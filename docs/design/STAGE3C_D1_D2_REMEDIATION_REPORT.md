# Stage 3C — D1/D2 Targeted Remediation Report

**Repository:** `Momegaii/intel_win_onlinux`
**Baseline:** `master` @ `2514e43156ebc9ea3ce8152dc8fe51d0b4144a2a`
**Implementation commit:** `c9598ef9c094b1c54ed681a94debec64b43dd7c9`
**Date:** 2026-11-21 UTC
**Status:** **D1/D2 REMEDIATED — READY FOR INDEPENDENT REVIEW**
**Stage 3C closure:** **NOT CLAIMED** — Check 15 physical Haswell evidence remains unavailable.

This pass changes only the D1/D2 ownership/accounting paths, their regression
tests, and evidence. It does not activate a physical GPU, unbind i915, access
MMIO, change PCI ownership, or implement Stage 3D.

---

## 1. D1 — VA reservation accounting corruption

### 1.1 Root cause

The backend used one global `va_reservations` counter but did not record which
allocation owned each increment.

Before the fix:

- In the post-publication map-error branch (`src/native_dma_backend.cpp`),
  `bind_cb_impl()` set `allocation->bound = true`, set `allocation->va`, and
  incremented `populated_ptes`, but did not increment `va_reservations`.
- In the successful-map path, `bound`/`va` were set before `check_inactive()`,
  `transition(PtSynchronized)`, `transition(ReadyInactive)`, and the
  `bind_fail_after_map` check; several of those paths could return or quarantine
  before the later `va_reservations` increment.
- `complete_unbind()` unconditionally decremented the global
  `va_reservations` counter whenever it released a VA.

Therefore a failed bind could leave a live PTE and a quarantined allocation
without a VA reservation. `recover_binding()` would then consume another
allocation's reservation. The first allocation became un-unbindable, and
`close_all()`/provider removal failed with `-EBUSY`.

### 1.2 Ownership invariant

The invariant is now explicit:

- An allocation owns a VA reservation when the backend has published PTEs for
  it, or when the PPGTT map failed but PTEs remain published and the allocation
  is quarantined.
- Ownership is recorded per allocation in `Allocation::va_reserved`.
- `va_reservations` is incremented at that ownership point, before any failure
  return that can leave the allocation quarantined.
- `complete_unbind()` decrements the counter only when
  `allocation.va_reserved` is true. An allocation that never recorded
  ownership cannot consume another allocation's counter.
- `va_reserved` is cleared only after the decrement succeeds, and is reset at
  the start of every bind and after a completed unbind.

### 1.3 Code changes

| File | Change |
|---|---|
| `include/idw/native_dma_backend.hpp` | Added `bool va_reserved` to the private `Allocation` state. |
| `src/native_dma_backend.cpp` | Reset `va_reserved` on bind; increment `va_reservations` immediately after `bound`/`va` are established in both post-map branches; removed the late duplicate increment; `complete_unbind()` releases only an owned reservation; reset ownership on completion. |

The map-error branch now increments the VA counter before `populated_ptes` and
before `quarantine()`. The success branch does the same immediately after
`transition(PgttPopulated)` and `bound`/`va` assignment, before
`check_inactive()`, state transitions, or `bind_fail_after_map`.

If `add_counter()` itself fails, the allocation is quarantined and
`va_reserved` remains false; cleanup will not decrement a counter that was
never incremented. This preserves fail-closed accounting for the unreachable
overflow case.

### 1.4 Regression test

Added `test_post_map_sync_failure_ownership()` in
`tests/native-dma-fault-test.cpp`.

Sequence:

1. Bind allocation 1 at `0x20000`; this creates and synchronizes a page table.
2. Create allocation 2 in the same PPGTT PDE.
3. Set `FaultPlan::sync_page_table_fail = true`.
4. Bind allocation 2 at `0x21000`; the existing table is used, the PTE is
   published, and synchronization fails.
5. Assert `bind` returns `-EUCLEAN`, allocation 2 is `Quarantined`, and
   `destroy(2)` returns `-EBUSY`.
6. Clear the fault and `recover_binding(2)`; assert it returns 0 and
   allocation 2 returns to `BackingAllocated`.
7. Unbind and destroy allocation 1 successfully.
8. Rebind allocation 2, then unbind and destroy it successfully.
9. Call `shutdown()` and `close_all()`; assert all six counters are zero.
10. Assert provider removal succeeds after references are released.

The test prints BEFORE, FAILURE, AFTER_RECOVER, and RECOVERY snapshots.

---

## 2. D2 — Provider finalization failure handling

### 2.1 Root cause

`idw_edu_probe()` allocated `struct idw_edu_test` with `kzalloc()`. The struct
embeds the provider and page-table adapter. On the `fail_provider` path, the
code called `idw_dma_provider_fini()` but ignored its return value, then
unconditionally `kfree(test)`.

`idw_dma_provider_fini()` deliberately returns `-EBUSY` and sets
`removal_blocked` when pages, resources, VMA references, or page-table
accounting remain. The provider API contract says the caller must retain or
quarantine the provider in that case. Freeing `test` after a refusal invalidates
the embedded provider, its lists, its pages/resources, and the adapter state
while the provider still holds a device reference.

The probe owns `test`; after a failed probe, no successful `remove()` callback
owns it. Therefore `test` may be freed only after both adapter and provider
finalization succeed. If either refuses, retaining `test` is the safe
fail-closed behavior.

### 2.2 Code changes

| File | Change |
|---|---|
| `kernel/idw_edu_dma_test_main.c` | Added `idw_edu_provider_fini_checked()`; it calls `idw_dma_provider_fini()` and clears `provider_ready` only on success. |
| `kernel/idw_edu_dma_test_main.c` | `fail_provider` now records adapter/provider finalization failure in `owner_retained`, logs the refusal, performs PCI cleanup, and skips `kfree(test)` while cleanup remains incomplete. |
| `kernel/idw_edu_dma_test_main.c` | Extended the QEMU provider-removal self-test to use a `struct idw_edu_test` owner, assert `provider_ready` and `removal_blocked` on refusal, release the outstanding resource/page/VMA reference, and verify successful finalization afterward. |

The retained owner is an explicit quarantine/leak on a failed probe. There is
no post-probe recovery callback, so the safe behavior is to retain rather than
free memory still referenced by the provider. Normal `remove()` refusal and
recovery remain handled by `idw_edu_remove()`.

### 2.3 D2 test evidence

The QEMU self-test exercises provider finalization with an outstanding resource
and VMA reference, then with an outstanding page. Both attempts return
`-EBUSY`, leave `provider_ready` true, and set `removal_blocked`. After the
reference is released, finalization returns 0. QEMU serial records:

```text
IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release
IDW_QEMU_DMA: provider finalization refusal retained owner and recovered after release
```

---

## 3. Six-counter snapshots

Order: `allocated_pages / mapped_pages / populated_ptes / pt_pages / va_reservations / quarantined_pages`.

| Stage | Counters | Meaning |
|---|---|---|
| D1_BEFORE | `2/2/1/1/1/0` | First allocation bound; second allocated but unbound; one page table exists. |
| D1_FAILURE | `2/2/2/1/2/1` | Second PTE published, second sync failed, second quarantined, both VA reservations owned. |
| D1_AFTER_RECOVER | `2/2/1/1/1/0` | Second PTE cleared and second reservation released; first remains bound and owns its reservation. |
| D1_RECOVERY | `0/0/0/0/0/0` | Both allocations destroyed; page table released; provider cleanup complete. |

Raw QEMU serial:

```text
COUNTERS D1_BEFORE alloc=2 mapped=2 ptes=1 pt=1 va=1 q=0
COUNTERS D1_FAILURE alloc=2 mapped=2 ptes=2 pt=1 va=2 q=1
COUNTERS D1_AFTER_RECOVER alloc=2 mapped=2 ptes=1 pt=1 va=1 q=0
COUNTERS D1_RECOVERY alloc=0 mapped=0 ptes=0 pt=0 va=0 q=0
```

The failure snapshot is the key regression assertion: with the old code,
`va_reservations` was 1, so recovery consumed the first allocation's
reservation. With the fix, it is 2, and recovery releases only the second.

---

## 4. Validation

All commands ran from `master` with implementation commit `c9598ef`.

| Command | Exit | Result | Raw log | SHA-256 |
|---|---:|---|---|---|
| `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build CC=cc` | 0 | PASS | `tests/qemu-dma/d1-d2-remediation-2026-11-21/kernel-build.log` | `d2186c42eeb693d15018b49701f84b6422d1aa4a6da984ecd4fe4200dce627e2` |
| `make contract-test backend-contract-test i915-resource-test i915-memory-backend-test gpu-submission-test memory-manager-test host-memory-backend-test native-dma-backend-test native-dma-fault-test native-dma-backend-test-static native-dma-fault-test-static abi-test ppgtt-model-test pt-dma-adapter-test ggtt-allocator-test haswell-gtt-hw-test device-lifecycle-test cli-test` | 0 | PASS for executed tests; native DMA tests SKIP without `/dev/idw-edu-dma` | `tests/qemu-dma/d1-d2-remediation-2026-11-21/software-tests.log` | `755f6bb4c51d192630d27e8493d3411c59bc09171bc0a4757ad13fae266a3fdb` |
| `make ppgtt-model-test` | 0 | PASS | `tests/qemu-dma/d1-d2-remediation-2026-11-21/ppgtt-model-test.log` | `3d3b82b34484d9e5906e6496c2d573673fb20f71adb657402e22802961429218` |
| `make native-dma-fault-test` | 0 | SKIP outside QEMU; not a pass | `tests/qemu-dma/d1-d2-remediation-2026-11-21/native-dma-fault-test.log` | `827412a31983606de1c2d900071515e04aec7abff507384619bcce7e467f25cd` |
| `make stage3a-sanitize-test` | 0 | PASS | `tests/qemu-dma/d1-d2-remediation-2026-11-21/stage3a-sanitize.log` | `73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294` |
| `KDIR=/lib/modules/6.12.117+deb13-amd64/build IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 make qemu-dma-test` | 0 | PASS; QEMU status 33 | `qemu-dma-test.log`; raw `qemu-dma-run.log` | `cf31a587d4b9e5b0cf63e1a60036e7b04d6baf4a3769bf6ea43b356e105adc90`; raw `d1523bc0e537adeb8cf3decdaccb2d6fdc5ee1a5e2381106367892e62185a439` |
| `make test` | **2** | FAIL only at `tests/smoke.sh`: no Intel render node | `tests/qemu-dma/d1-d2-remediation-2026-11-21/make-test.log` | `27ebc1362554465bbece3e88d8e4b77f79cab78425b6d9c21312c33ca2632e76` |

QEMU serial contains:

```text
IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release
IDW_QEMU_DMA: provider finalization refusal retained owner and recovered after release
PASS: native DMA backend fault injection, teardown retry, VMA lifetime, and ownership counters
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
QEMU exit status: 33
```

The full `make test` failure is environmental: `tests/smoke.sh` requires an
Intel DRM render node and `/dev/dri`; neither exists in this container. All
software tests before the smoke test passed. Native DMA tests skip outside QEMU;
the QEMU EDU run is the actual native DMA fault/VMA test pass.

---

## 5. Changed files

Implementation and test changes:

- `include/idw/native_dma_backend.hpp`
- `src/native_dma_backend.cpp`
- `kernel/idw_edu_dma_test_main.c`
- `tests/native-dma-fault-test.cpp`

Documentation:

- `docs/design/STAGE3C_D1_D2_REMEDIATION_REPORT.md` (this report)
- `docs/design/STAGE3C_REMEDIATION_REPORT.md` (post-remediation status note)

Evidence:

- `tests/qemu-dma/d1-d2-remediation-2026-11-21/`
- `tests/qemu-dma/post-merge-2026-11-21/` (pre-remediation verification)

No unrelated source, build-system, UAPI, or hardware-path file changed.

---

## 6. Remaining limitations

- **Check 15 is still INSUFFICIENT EVIDENCE.** The host has no `8086:0412`
  Haswell GPU, no `/dev/dri`, and no i915 binding. QEMU EDU is virtual-device
  DMA only; it is not physical Haswell evidence.
- **Full `make test` is not green in this container** because no Intel render
  node exists. This is not caused by D1/D2.
- **D2 retained owner is a deliberate quarantine on probe failure.** If probe
  cleanup refuses finalization, `struct idw_edu_test` is retained rather than
  freed. There is no post-probe recovery callback; the provider remains
  fail-closed. Normal remove-path refusal and recovery are tested separately.
- **No Stage 3C closure is claimed.** This is targeted remediation for
  independent review, not an independent acceptance or physical validation.

---

## 7. Git state

Implementation commit: `c9598ef9c094b1c54ed681a94debec64b43dd7c9`.

The final documentation commit follows this report. No push or merge was
performed. After the implementation commit, the working tree contains only the
documentation updates; both evidence directories are part of `c9598ef`.
