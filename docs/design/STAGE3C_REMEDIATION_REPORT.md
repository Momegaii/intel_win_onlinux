# Stage 3C — Post-Merge Verification and Closure Report

**Repository:** `Momegaii/intel_win_onlinux`
**Report date:** 2026-11-21 UTC
**Verification type:** Post-merge, read-only source review, test reproduction, and evidence classification
**Merged baseline:** `master` at `2514e43156ebc9ea3ce8152dc8fe51d0b4144a2a`
**Stage 3C remediation commit:** `5f3d4ceb3ba023110634ac0a3af1c3e6adde1231` (included in merged history)
**Original verification recommendation:** **NOT_READY_FOR_CLOSURE** — one confirmed lifecycle/counter defect and one missing failure-path test remained at the time of this snapshot.

**Post-remediation status (2026-11-21):** D1 and D2 were fixed in commit
`c9598ef9c094b1c54ed681a94debec64b43dd7c9`. Checks 2, 10, and 16 now pass;
Check 15 was subsequently satisfied by a read-only bare-metal host run
(`tests/physical/evidence-2026-11-21-host/`). See
`docs/design/STAGE3C_D1_D2_REMEDIATION_REPORT.md`. **Stage 3C is not closed.**

> This is a verification report. No implementation source, test source, hardware state, Git history, or remote branch was changed during this pass.

---

## 1. Merged baseline and repository state

### 1.1 State before verification

| Item | Value |
|---|---|
| Branch at task start | `fix/stage3c-remediation` |
| HEAD at task start | `5f3d4ceb3ba023110634ac0a3af1c3e6adde1231` |
| Working tree before evidence/report writes | Clean |
| Merge commit | `2514e43156ebc9ea3ce8152dc8fe51d0b4144a2a` |
| Default branch | `master` (repository has no `main` branch) |
| `5f3d4ce` in merged history | Yes; `git merge-base --is-ancestor 5f3d4ce HEAD` returned 0 |

The verification then checked out `master` and fast-forwarded it locally to
`origin/master` (`fa2e918..2514e43`). No push or remote merge was performed.
The working tree was clean at `2514e43` before the evidence directory and this
report were written.

### 1.2 Merged history

```text
2514e43 Merge pull request #6 from fix/stage3c-remediation
5f3d4ce Harden Stage 3C VMA teardown and PPGTT retry paths
403381b Remediate Stage 3C native DMA audit findings
fa2e918 Add native DMA resource backing through MemoryManager
```

The merged change from `403381b` to `2514e43` includes the Stage 3C provider,
UAPI, backend, MemoryManager, PPGTT, test, and evidence files. The remediation
changes are present; the confirmed defect below is in the merged code, not a
missing commit.

### 1.3 Files created or modified by this verification

Created:

- `tests/qemu-dma/post-merge-2026-11-21/hardware-detection.log`
- `tests/qemu-dma/post-merge-2026-11-21/kernel-build.log`
- `tests/qemu-dma/post-merge-2026-11-21/software-tests.log`
- `tests/qemu-dma/post-merge-2026-11-21/ppgtt-model-test.log`
- `tests/qemu-dma/post-merge-2026-11-21/native-dma-fault-test.log`
- `tests/qemu-dma/post-merge-2026-11-21/stage3a-sanitize.log`
- `tests/qemu-dma/post-merge-2026-11-21/qemu-dma-test-stdout.log`
- `tests/qemu-dma/post-merge-2026-11-21/qemu-dma-run.log`
- `tests/qemu-dma/post-merge-2026-11-21/qemu-dma-run-before.log` (preserved pre-existing `build/qemu-dma/run.log`)
- `tests/qemu-dma/post-merge-2026-11-21/make-test.log`
- `tests/qemu-dma/post-merge-2026-11-21/defect-repro-qemu.log`
- `tests/qemu-dma/post-merge-2026-11-21/README.md` (concise claim-to-log/hash index)

Modified:

- `docs/design/STAGE3C_REMEDIATION_REPORT.md` (this report)

No implementation or test file was modified. Existing committed raw logs were
not overwritten. After report generation, `git status --porcelain` shows only:

```text
 M docs/design/STAGE3C_REMEDIATION_REPORT.md
?? tests/qemu-dma/post-merge-2026-11-21/
```

No commit or push was made.

---

## 2. Hardware detection and evidence classification

Read-only host detection is recorded in
`tests/qemu-dma/post-merge-2026-11-21/hardware-detection.log`.

| Observation | Result |
|---|---|
| Target Haswell PCI ID `8086:0412` | **Absent** |
| Target PCI address `0000:00:02.0` | Absent |
| Intel DRM render node `/dev/dri` | Absent |
| `i915` in `/proc/modules` | Absent |
| `/sys/module/i915` | Absent |
| QEMU EDU `1234:11e8` on host | Absent |
| `lspci` | Not installed; sysfs used instead |
| PCI devices present | Intel chipset IDs `8086:1237`, `8086:7110`, `8086:7113`; non-target devices `1ae0:0042`, `1af4:1005`, `1ae0:001f` |

Classifications:

- **CPU_ONLY** — PPGTT model and other software-only tests.
- **QEMU_EDU_DMA** — only the QEMU EDU TCG harness; this is virtual-device DMA.
- **TARGET_GPU_NOT_PRESENT** — PCI device `8086:0412` is absent.
- **PHYSICAL_I915_ASSISTED** — not available at report time; a later Check 15
  host run recorded i915 binding, but no i915-assisted execution test.
- **INDEPENDENT_PHYSICAL_HASWELL** — not available.

QEMU EDU does not emulate the target Haswell GPU. The QEMU result is not
physical Haswell evidence. At report time, Check 15 was unresolved pending
read-only evidence from a physical host showing the Haswell GPU and its
`i915` binding. That binding evidence was subsequently collected; see
`tests/physical/evidence-2026-11-21-host/`.

---

## 3. Source-review findings

### 3.1 VMA lifetime — PASS for the reviewed paths

| Requirement | Source evidence | Result |
|---|---|---|
| Resource VMA reference accounting | `idw_edu_resource_vm_open()` increments `resource->mappings`; `idw_edu_resource_vm_close()` decrements it; both hold `provider->vma_refs` and module references. `kernel/idw_edu_dma_test_main.c:774-795`. | PASS |
| Page-table VMA reference accounting | `idw_edu_pt_vm_open()` / `idw_edu_pt_vm_close()` mirror the resource path and use `page->mappings`. `kernel/idw_edu_dma_test_main.c:802-823`. | PASS |
| Atomic decrement and underflow protection | `idw_edu_vma_put()` uses `atomic_dec_if_positive()` and returns without `module_put()` on underflow. `kernel/idw_edu_dma_test_main.c:765-771`. | PASS |
| Mapping after file descriptor closure | Resource and page-table VMAs hold a file reference; `idw_edu_release()` cannot run while the VMA is live. QEMU `test_resource_vma_lifetime` closes the fd and still reads/writes the mapping. | PASS |
| Fork-shared mapping | Child observes `vma_refs == 2`, writes, unmaps, and the parent retains `vma_refs == 1`. | PASS |
| Multiple mappings | A second mmap raises `vma_refs` to 2 and `FREE`/`FREE_PT` returns `EBUSY`. | PASS |
| VMA split / partial unmap | Removing the middle page leaves two halves and `vma_refs == 2`; both halves must close before free succeeds. | PASS |
| Final release | `vma_refs` reaches zero, `FREE`/`FREE_PT` succeeds, and provider removal then succeeds in the tested path. | PASS |

The VMA tests are in `tests/native-dma-fault-test.cpp`
(`test_resource_vma_lifetime`, `test_page_table_vma_lifetime`) and pass under
the QEMU fault test. The QEMU raw log contains the fault-test PASS line.

### 3.2 Teardown and retry — FAIL for one post-map failure path

The merged code has explicit progress flags and retry logic:

- `TeardownProgress` exposes `ptes_cleared`, `ptes_accounted`,
  `invalidation_done`, `va_released`, `dma_unmapped`, `backing_released`, and
  `complete` (`include/idw/native_dma_backend.hpp`).
- `unbind_cb_impl()` retries poisoned synchronization, checks
  `idw_hsw_ppgtt_range_cleared()`, clears PTEs once, accounts `populated_ptes`
  once, completes invalidation, and releases the VA reservation once
  (`src/native_dma_backend.cpp:558-636`).
- `destroy_backing_cb_impl()` avoids a second `munmap`/`FREE` after a partial
  failure (`src/native_dma_backend.cpp:332-397`).
- `close_all()` completes pending invalidation, checks inactivity, destroys the
  model with `hardware_quiesced=false`, retries remaining page-table release,
  and refuses success while counters remain (`src/native_dma_backend.cpp:1120-1160`).
- Kernel provider free paths are idempotent and fail closed
  (`kernel/idw_dma_memory.c:45-145`, `236-330`, `512-565`).

However, the post-map failure path has a confirmed defect. See section 4.

### 3.3 PPGTT poison and recovery — PASS for reviewed model paths

- `idw_hsw_ppgtt_map()` marks `sync_failed` and `poisoned` on PTE-table sync
  failure (`src/haswell_ppgtt_model.c:500-515`).
- `idw_hsw_ppgtt_unmap()` clears PTEs, marks the range dirty before syncing,
  and records `sync_failed`/`poisoned` on sync failure
  (`src/haswell_ppgtt_model.c:525-577`).
- `idw_hsw_ppgtt_range_cleared()` detects whether a VA range still has valid
  PTEs (`src/haswell_ppgtt_model.c:583-611`).
- `idw_hsw_ppgtt_retry_sync()` clears poison only after all existing page
  tables resynchronize (`src/haswell_ppgtt_model.c:613-638`).
- `tests/ppgtt-model-test.c` covers map sync failure, unmap sync failure,
  poison clearing, and range-cleared queries. It passes.

### 3.4 Provider lifecycle — PASS for normal refusal/recovery, with a defect consequence

- `idw_dma_provider_fini()` returns `-EBUSY` and sets `removal_blocked` while
  pages, resources, VMA references, or provider counters remain
  (`kernel/idw_dma_memory.c:110-145`).
- The probe holds a resource, page-table page, and VMA reference, proves each
  removal refusal, releases each, and proves removal succeeds.
- `idw_edu_remove()` deregisters the misc device, waits for the opener, checks
  adapter/provider state, and quarantines instead of freeing outstanding pages
  (`kernel/idw_edu_dma_test_main.c:1344-1394`).
- QEMU log: `provider removal refused outstanding refs and recovered after release`
  and `remove released all provider resources result=0`.

The normal provider-removal contract passes. The defect in section 4 can leave a
live allocation that cannot be released, which then prevents provider removal.

---

## 4. Confirmed defect D1 — VA reservation counter is not incremented on post-map failure

### 4.1 Exact source locations

- `src/native_dma_backend.cpp:451-460` — `bind_cb_impl()` handles a PPGTT map
  error with PTEs still valid: it sets `bound = true`, sets `va`, increments
  `populated_ptes`, quarantines, and returns `-EUCLEAN`, but does **not**
  increment `va_reservations`.
- `src/native_dma_backend.cpp:469-501` — the successful-map path sets
  `bound = true` and `va` at lines 469-470, then can return early from
  `add_counter(populated_ptes)`, `check_inactive()`, `transition(...)`, or the
  `va_reservations` overflow check before the counter is incremented at line
  498. Any such return leaves `bound = true` with no VA reservation recorded.
- `src/native_dma_backend.cpp:533-536` — `complete_unbind()` unconditionally
  decrements the global `va_reservations` counter when releasing the VA.

### 4.2 Reproduction

A temporary QEMU diagnostic harness (outside the repository; no test or source
file was modified) used two one-page allocations in the same PPGTT PDE:

1. Bind allocation 1 at `0x20000` with no fault. This creates and synchronizes
   a page table. Counters after bind:
   `allocated=1 mapped=1 ptes=1 pt=1 va=1 quarantine=0`.
2. Create allocation 2 and set `FaultPlan::sync_page_table_fail = true`.
3. Bind allocation 2 at `0x21000`. The page table already exists, so the map
   writes its PTE and then fails synchronization after publication. `bind`
   returns `-EUCLEAN`; counters become
   `allocated=2 mapped=2 ptes=2 pt=1 va=1 quarantine=1`.
4. Clear the fault and call `recover_binding(allocation 2)`. It returns 0, but
   it consumes allocation 1's VA reservation:
   `allocated=2 mapped=2 ptes=1 pt=1 va=0 quarantine=0`.
5. `unbind(allocation 1)` then fails with `-EIO` at the
   `sub_counter(va_reservations, 1)` check; `destroy` returns `-EBUSY`,
   `close_all` returns `-EBUSY`, and module removal reports
   `remove quarantined outstanding resources adapter=0 provider=-16 result=-16`.

Raw reproduction log:
`tests/qemu-dma/post-merge-2026-11-21/defect-repro-qemu.log`
(SHA-256 `5233abbfc9b99b885f72eb894bd46f94c676330b5b02577a46c1001f8807c871`).

The standard `test_sync_failure()` does not reach this path because its
`sync_page_table_fail` fault fires during `allocate_table()` before PTE
publication, so `idw_hsw_ppgtt_range_cleared()` is true and the post-map branch
is not taken (`tests/native-dma-fault-test.cpp:205-222`;
`src/haswell_ppgtt_model.c:195-200`).

### 4.3 Impact

- A failed bind after PPGTT publication can permanently block cleanup.
- The global VA reservation counter can be consumed by the wrong allocation.
- `unbind`, `destroy`, `close_all`, and provider removal can all fail with
  `-EBUSY`/`-EIO` after an otherwise recoverable bind failure.
- The failure is a lifecycle and accounting defect, not a test-only issue.

### 4.4 Proposed minimal fix (not implemented)

Increment `va_reservations` immediately after setting `allocation->bound` and
`allocation->va` in both post-map branches, before any return that can leave the
allocation quarantined. Remove the later duplicate increment. Equivalently, add
an explicit `va_reserved` progress flag and make `complete_unbind()` release
only a reservation that was actually recorded.

A regression test should bind one allocation to create the page table, then
bind a second allocation in the same PDE with a fault that fails only the
second table synchronization, and assert that both allocations can be unbound
and that all six counters return to zero.

### 4.5 Secondary cleanup observation D2 (not exercised)

`kernel/idw_edu_dma_test_main.c:1310-1337` ignores the return value of
`idw_dma_provider_fini()` in the probe failure path and then executes
`kfree(test)`. If a probe self-test leaves a page, resource, or adapter
reference, provider teardown returns `-EBUSY`; the code then frees the owner
while the provider/device reference remains. No UAF was observed, but this is a
leak/fail-open cleanup path. Proposed minimal fix: do not `kfree(test)` when
`idw_dma_provider_fini()` fails; preserve the provider state, report `-EBUSY`,
and let the probe fail closed.

---

## 5. Six-counter matrix

Counter order in each cell:
`allocated_pages / mapped_pages / populated_ptes / pt_pages / va_reservations / quarantined_pages`.
`—` means the merged test or log does **not** assert that counter at that
stage. It is not an inferred zero. No values are invented.

| Test / path | BEFORE | FAILURE | AFTER | RECOVERY | Evidence |
|---|---|---|---|---|---|
| `test_native_dma_backend` (happy path) | `0/0/0/0/0/0` | n/a | after create: `3/3/0/0/0/0`; after bind: `3/3/3/0/1/0`; after unbind: `3/3/0/0/0/0` | after destroy/close: `0/0/0/0/0/0` | `tests/native-dma-backend-test.cpp:46-51,71-73,119-140` |
| `test_page_table_failure` | — | `—/—/0/—/0/—` | — | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:155-180` |
| `test_mmap_failure` | — | — | — | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:182-202` |
| `test_sync_failure` | — | — | — | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:205-222` |
| `test_destroy_retry` | — | `3/—/—/—/—/3` | — | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:225-252` |
| `test_unbind_retry` | — | `—/—/0/—/1/3` | after recover: `—/—/—/—/—/0` | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:254-289` |
| `test_bind_failure_after_map` | — | `—/—/3/—/1/3` | — | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:291-322` |
| `test_unbind_sync_failure_retry` | — | `—/—/0/—/1/3` | after recover: `—/—/0/—/0/0` | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:324-362` |
| `test_destroy_after_unmap_retry` | — | `1/—/—/—/—/1` | — | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:364-395` |
| `test_page_table_release_retry` | — | `—/—/—/1/—/1` | — | `0/0/0/0/0/0` | `tests/native-dma-fault-test.cpp:397-422` |
| D1 reproduction (two allocations, post-publication sync failure) | after bind 1: `1/1/1/1/1/0` | after failed bind 2: `2/2/2/1/1/1` | after `recover_binding(2)`: `2/2/1/1/0/0` | final: `2/2/0/1/0/1` — **not zero** | `tests/qemu-dma/post-merge-2026-11-21/defect-repro-qemu.log` |

### 5.1 Counter evidence gaps

- Most failure tests assert only the counters relevant to their fault and the
  final `counters_zero()`; they do not capture a full six-counter snapshot at
  every stage.
- `test_sync_failure()` asserts no intermediate counter snapshot. It also does
  not exercise post-publication synchronization failure.
- No merged test asserts a full six-counter BEFORE/FAILURE/AFTER/RECOVERY
  sequence for every failure path. The D1 diagnostic is the only full sequence
  available, and it shows a nonzero final state.
- The final nonzero counters in D1 are not a test artifact: `allocated=2`,
  `mapped=2`, `pt=1`, and `quarantine=1` remain after `close_all` returns
  `-EBUSY`.

---

## 6. Verification commands and results

All commands were run from the merged `master` tree at `2514e43`. The concise
claim-to-log/hash index is also available at
`tests/qemu-dma/post-merge-2026-11-21/README.md`.

| # | Command | Exit | Result | Raw log | SHA-256 |
|---:|---|---:|---|---|---|
| 1 | `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build CC=cc` | 0 | PASS | `tests/qemu-dma/post-merge-2026-11-21/kernel-build.log` | `7049243fd20b254a4582874269b7358c0198ab75751205bdc4735788f0d44009` |
| 2 | `make contract-test backend-contract-test i915-resource-test i915-memory-backend-test gpu-submission-test memory-manager-test host-memory-backend-test native-dma-backend-test native-dma-fault-test native-dma-backend-test-static native-dma-fault-test-static abi-test ppgtt-model-test pt-dma-adapter-test ggtt-allocator-test haswell-gtt-hw-test device-lifecycle-test cli-test` | 0 | PASS for executed tests; `native-dma-backend-test` and `native-dma-fault-test` **SKIPPED** (no `/dev/idw-edu-dma`) | `tests/qemu-dma/post-merge-2026-11-21/software-tests.log` | `755f6bb4c51d192630d27e8493d3411c59bc09171bc0a4757ad13fae266a3fdb` |
| 3 | `make ppgtt-model-test` | 0 | PASS | `tests/qemu-dma/post-merge-2026-11-21/ppgtt-model-test.log` | `3d3b82b34484d9e5906e6496c2d573673fb20f71adb657402e22802961429218` |
| 4 | `make native-dma-fault-test` | 0 | **SKIPPED** — `/dev/idw-edu-dma` absent; not reported as passing | `tests/qemu-dma/post-merge-2026-11-21/native-dma-fault-test.log` | `827412a31983606de1c2d900071515e04aec7abff507384619bcce7e467f25cd` |
| 5 | `make stage3a-sanitize-test` | 0 | PASS | `tests/qemu-dma/post-merge-2026-11-21/stage3a-sanitize.log` | `73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294` |
| 6 | `KDIR=/lib/modules/6.12.117+deb13-amd64/build IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 make qemu-dma-test` | 0 | PASS; QEMU exit status 33 | `tests/qemu-dma/post-merge-2026-11-21/qemu-dma-test-stdout.log`; raw serial: `qemu-dma-run.log` | stdout `9949eea93150bef61e10486ac1cdda5219b65c83c4083992942c4f8410c5da04`; raw `bbb97c11e177cdc50ed38113b7832bd15634645047b4ccd7f4b7af3283d0ee7e` |
| 7 | `make test` | **2** | FAIL at `./tests/smoke.sh`; preceding software tests passed; native fault tests skipped | `tests/qemu-dma/post-merge-2026-11-21/make-test.log` | `27ebc1362554465bbece3e88d8e4b77f79cab78425b6d9c21312c33ca2632e76` |
| 8 | Diagnostic QEMU defect reproduction (temporary initramfs; no repo test/source change) | 35 | FAIL as expected for D1; serial shows `bind2=-117`, `recover_binding2=0`, `unbind1=-5`, `close_all=-16` | `tests/qemu-dma/post-merge-2026-11-21/defect-repro-qemu.log` | `5233abbfc9b99b885f72eb894bd46f94c676330b5b02577a46c1001f8807c871` |

### 6.1 Full `make test` failure detail

The full suite reaches the smoke test and fails only because the container has
no Intel DRM render node:

```text
./tests/smoke.sh
IDW — Intel GPU Properties
==========================
No Intel DRM render node found.
Check: lspci -nnk | grep -A3 -i vga and ls -l /dev/dri
FAIL: no Intel render node found
make: *** [Makefile:189: test] Error 2
```

The preceding software tests in the same log passed. The two native DMA tests
that require `/dev/idw-edu-dma` were skipped, not passed. The QEMU EDU run is
the applicable pass for those tests.

### 6.2 QEMU evidence

QEMU EDU TCG log includes:

```text
IDW_QEMU_DMA: page-table DMA layout verified
IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified
IDW_QEMU_DMA: backing allocation/mapping fault injection verified
IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release
PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup
PASS: native DMA backend fault injection, teardown retry, VMA lifetime, and ownership counters
IDW_QEMU_DMA: remove released all provider resources result=0
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
QEMU exit status: 33
```

The pre-existing `build/qemu-dma/run.log` was copied to
`qemu-dma-run-before.log` before the new run. Existing committed evidence logs
were not overwritten.

---

## 7. Seventeen-point Stage 3C review

| # | Criterion | Verdict | Source / test / log evidence | Limitation |
|---:|---|---|---|---|
| 1 | Resource state machine and transition validation | **PASS** | `NativeDmaMemoryBackend::ResourceState` and `transition()` (`src/native_dma_backend.cpp:100-160`); happy-path and fault tests assert states; QEMU fault PASS. | The D1 counter bug does not create an illegal transition, but it corrupts recovery accounting. |
| 2 | Independent ownership counters and leak assertions | **FAIL** | Six counters exist (`include/idw/native_dma_backend.hpp:29-36`). `test_native_dma_backend` asserts all six on the happy path. D1 leaves `va_reservations` wrong and final counters nonzero. | Confirmed defect D1; most failure tests lack full six-counter snapshots. |
| 3 | Per-page DMA mapping and unmapping symmetry | **PASS** | `idw_dma_resource_alloc()` rollback, `idw_dma_resource_unmap_pages()`, `idw_dma_resource_free_pages()` (`kernel/idw_dma_memory.c:45-330`); probe backing allocation/mapping faults; QEMU log. | Provider-level only; no physical GPU. |
| 4 | DMA mask configuration and address limits | **PASS** | Probe calls 39-bit streaming and 40-bit coherent masks (`kernel/idw_edu_dma_test_main.c:1260-1266`); QEMU log: `accepted 39-bit streaming and 40-bit coherent masks`. | QEMU device only. |
| 5 | DMA synchronization and mapping direction | **PASS** | `idw_dma_resource_sync()` direction compatibility and DMA API direction (`kernel/idw_dma_memory.c:397-490`); probe rejects incompatible direction; QEMU log. | QEMU device only. |
| 6 | Safe userspace mapping | **PASS** | `idw_edu_mmap()`, resource/PT VMA ops, `mappings` and `vma_refs` (`kernel/idw_edu_dma_test_main.c:757-947`); `test_resource_vma_lifetime` and `test_page_table_vma_lifetime` pass under QEMU. | No physical GPU; VMA tests require QEMU device. |
| 7 | Existing Haswell PPGTT encoder reuse and DMA validation | **PASS** | `idw_hsw_ppgtt_encode_pte/pde`, 39-bit/40-bit validation, `idw_hsw_ppgtt_map()`; `tests/ppgtt-model-test.c`; QEMU page-table DMA log. | Software/QEMU only. |
| 8 | Explicit cacheability policy | **PASS** | PTE UC encoding; `pgprot_writecombine()` in resource mmap; `dma_mmap_coherent()` for PT VMA (`kernel/idw_edu_dma_test_main.c:900-940`). | Software/QEMU only. |
| 9 | PPGTT model detachment before PT backing release | **PASS** | `close_all()` completes invalidation, checks inactive, destroys model with `false`, retries PT release (`src/native_dma_backend.cpp:1120-1160`); provider fini checks counters. | No physical GPU. |
| 10 | Reverse-order rollback and quarantine | **FAIL** | Retry flags and `range_cleared()` are present, but D1 shows a post-publication bind failure consumes the wrong VA reservation and leaves allocation 1 un-unbindable (`unbind1=-5`, `close_all=-16`). | Confirmed defect; standard tests do not reach the branch. |
| 11 | UAPI permissions and device lifecycle safety | **PASS** | One-opener `open`; `ops_lock` serializes ioctl/mmap/free/remove; VMA refs block free/removal; `release()` waits for VMA file refs (`kernel/idw_edu_dma_test_main.c:830-852`). | No physical GPU. |
| 12 | Provider removal with outstanding allocations | **PASS** (normal path) | `idw_dma_provider_fini()` refuses on pages/resources/VMA/counters (`kernel/idw_dma_memory.c:110-145`); probe refusal/recovery; QEMU `remove released all provider resources result=0`. | D1 can leave an allocation that cannot be released; this is a consequence tracked under Checks 2/10/16, not a failure of normal refusal. |
| 13 | Inactive-hardware invariant | **PASS** | `quiesce_cb()` checks fence and `check_inactive()`; `close_all()` calls `check_inactive()` and destroys with `hardware_quiesced=false` (`src/native_dma_backend.cpp:700-710,1120-1160`). | No physical GPU. |
| 14 | PPGTT evidence separation | **PASS** | QEMU log labels TCG; no physical Haswell claim; CPU-only PPGTT tests separate. | None beyond scope. |
| 15 | GGTT and hardware execution boundary | **PASS** (post-snapshot) | The snapshot host had `8086:0412` absent, no `/dev/dri`, and no i915; source has no Haswell MMIO/BAR/GGTT/ring operation. A later bare-metal host run recorded `0000:00:02.0`, `8086:0412`, `i915` bound, and `renderD128`. | Binding evidence is recorded; physical BAR/MMIO/GGTT/PPGTT/submission behavior remains untested. |
| 16 | Failure-path evidence | **FAIL** | Fault tests cover create, PT alloc, mmap, sync, bind-after-map, unbind invalidation/sync, destroy, resource free, PT free, VMA cases. No test covers post-publication sync failure with an existing table; D1 is therefore unguarded. | Missing regression test; D1 confirmed by diagnostic harness. |
| 17 | Evidence classification | **PASS** | `TARGET_GPU_NOT_PRESENT`, `CPU_ONLY`, `QEMU_EDU_DMA` classified; no `PHYSICAL_I915_ASSISTED` or `INDEPENDENT_PHYSICAL_HASWELL` claims. | Physical execution buckets remain empty; Check 15 binding evidence was recorded later. |

---

## 8. Outstanding limitations and closure decision

### 8.1 Unresolved criteria

- **Check 2** — counter accounting is not correct for post-publication bind
  failure.
- **Check 10** — recovery after that failure can corrupt another allocation's
  VA reservation and permanently block cleanup.
- **Check 16** — no merged regression test reaches the failing branch.
- **Check 15** — physical Haswell/i915 binding evidence was unavailable in the
  audit container; `TARGET_GPU_NOT_PRESENT`. A later bare-metal host run
  returned `PASS`; see `tests/physical/evidence-2026-11-21-host/`.

### 8.2 Recommendation

**Original snapshot: NOT_READY_FOR_CLOSURE.** D1/D2 have since been remediated
in `c9598ef`; see `docs/design/STAGE3C_D1_D2_REMEDIATION_REPORT.md`. Check 15
binding evidence has since been collected from a bare-metal host, but Stage 3C
is still not closed because the remaining lifecycle, failure-path, and
physical-execution checks still apply.

At the time of this snapshot, the merged implementation passed the normal
software, PPGTT, sanitizer, and QEMU EDU paths, but D1 violated the Stage 3C
teardown, counter, and recovery requirements. The missing regression test was
also a required-evidence gap. No code was changed during that verification, and
no physical Haswell claim was made.

Stage 3C must not be described as independently accepted or physically
validated. Re-audit the D1/D2 remediation and close the remaining failure-path
and physical-execution checks. The read-only Check 15 binding evidence is
recorded in `tests/physical/evidence-2026-11-21-host/`.
