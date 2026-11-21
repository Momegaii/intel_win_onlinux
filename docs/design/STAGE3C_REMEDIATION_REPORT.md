# Stage 3C — Remediation Report

**Repository:** `Momegaii/intel_win_onlinux`
**Audit remediated:** `docs/design/STAGE3C_17_POINT_PR_AUDIT.md`
**Baseline audited:** `fa2e91879b02526b4a31615ddf19561443d8e3cc`
**Remediation date:** 2026-11-20 UTC
**Result:** **READY FOR RE-AUDIT** (16 checks pass; physical i915 binding evidence remains unavailable in this container)

---

## 1. Summary

The Stage 3C findings were remediated in the kernel provider, UAPI, native DMA
backend, MemoryManager integration, and tests. The QEMU EDU harness now runs
both the happy-path and fault-injection programs. The only open item is the
environmental i915-binding evidence in Check 15: this container exposes no
Intel GPU, no `/dev/dri`, and no i915 module, so physical binding cannot be
proven here.

No physical Haswell BAR, MMIO, DMA, GPU submission, PPGTT activation, or PCI
ownership change was performed.

---

## 2. Seventeen-point status after remediation

| # | Check | Status | Remediation evidence |
|---:|---|---|---|
| 1 | Resource state machine and transition validation | **PASS** | `NativeDmaMemoryBackend::ResourceState` now has `New`, `VaReserved`, `BackingAllocated`, `DmaMapped`, `PgttPopulated`, `PtSynchronized`, `ReadyInactive`, `Destroying`, `Quarantined`, and `Released`. `transition()` rejects illegal edges. Tests assert `BackingAllocated -> ReadyInactive -> BackingAllocated`, quarantine, retry, and stale-handle rejection. |
| 2 | Independent ownership counters and leak assertions | **PASS** | `OwnershipCounters` tracks `allocated_pages`, `mapped_pages`, `populated_ptes`, `pt_pages`, `va_reservations`, and `quarantined_pages`. Tests assert zero only after model/provider teardown and assert nonzero quarantine after injected destroy/unbind failures. |
| 3 | Per-page DMA mapping and unmapping symmetry | **PASS** | `kernel/idw_dma_memory.c` exposes test-only `fail_alloc_at` and `fail_map_at`. The probe injects first/middle/last backing-page allocation and mapping failures and verifies the provider still reports exactly the two live resources and six mapped pages. |
| 4 | DMA mask configuration and address limits | **PASS** | Probe now calls `dma_set_mask(dev, DMA_BIT_MASK(39))` and `dma_set_coherent_mask(dev, DMA_BIT_MASK(40))`. `idw_dma_provider_init()` rejects a streaming mask below 39 bits. QEMU log: `accepted 39-bit streaming and 40-bit coherent masks`. |
| 5 | DMA synchronization and mapping direction | **PASS** | `idw_dma_resource_sync()` rejects a requested direction incompatible with the mapping direction and passes the requested direction to the DMA API. Probe allocates a `DMA_TO_DEVICE` resource, rejects `DMA_FROM_DEVICE` with `-EINVAL`, accepts `DMA_TO_DEVICE`, and frees it. |
| 6 | Safe userspace mapping | **PASS** | Resource and page-table VMAs now carry provider references and module references. A VMA holds a file reference, so `release()` cannot run while it is live; the provider cannot be removed until `vma_refs` reaches zero. `FREE`/`FREE_PT` reject live mappings. The QEMU fault test maps a resource, closes one fd, verifies the mapping still works, verifies `vma_refs == 1` and `FREE` returns `-EBUSY`, then unmaps and frees successfully. Resource VMAs use explicit write-combined protection. |
| 7 | Existing Haswell PPGTT encoder reuse and DMA validation | **PASS** | Existing PDE/PTE encoders and 39-bit/40-bit limits are unchanged. The native test still compares translated DMA addresses with real resource DMA addresses. |
| 8 | Explicit cacheability policy | **PASS** | PTE UC encoding remains explicit. Resource mmap now sets `pgprot_writecombine()`. Page-table mmap uses `dma_mmap_coherent()`. Documentation states this is software/QEMU evidence only. |
| 9 | PPGTT model detachment before PT backing release | **PASS** | `close_all()` checks inactivity, destroys the model with `hardware_quiesced=false`, then refuses success while page tables or counters remain. `idw_dma_provider_fini()` refuses while pages, resources, or VMAs remain. |
| 10 | Reverse-order rollback and quarantine | **PASS** | `unbind_cb_impl()` records `ptes_cleared`, can retry invalidation, and quarantines without silently freeing. `complete_unbind()` clears counters only after invalidation succeeds. Fault tests cover unbind failure, retry, and recovery. |
| 11 | UAPI permissions and device lifecycle safety | **PASS** | One-opener restriction remains. `ops_lock` serializes ioctl/mmap/free/remove. VMA references block provider removal. `release()` zaps mappings. VMA `open`/`close` hold module references. |
| 12 | Provider removal with outstanding allocations | **PASS** | `idw_dma_provider_fini()` returns `-EBUSY` and sets `removal_blocked` while any page, resource, or VMA remains. `idw_edu_remove()` quarantines and reports `-EBUSY` instead of freeing outstanding pages. QEMU remove succeeds only after all references are released. |
| 13 | Inactive-hardware invariant | **PASS** | `quiesce_cb()` validates the fence and calls `check_inactive()`. `close_all()` calls `check_inactive()` and destroys with `hardware_quiesced=false`; the literal `true` assertion was removed. |
| 14 | PPGTT evidence separation | **PASS** | QEMU TCG remains labeled virtual evidence. No physical Haswell claim was added. |
| 15 | GGTT and hardware execution boundary | **INSUFFICIENT EVIDENCE** | Source still contains no Haswell MMIO/BAR/GGTT/ring operation. This container has no `0000:00:02.0` in `/sys/bus/pci/devices`, no i915 module in `/proc/modules`, and no `/dev/dri`, so host binding cannot be verified here. `lspci` is not installed; the equivalent sysfs check is absent. Re-run `lspci -nnk -s 00:02.0` and the sysfs driver check on a GPU host. |
| 16 | Failure-path evidence | **PASS** | New `tests/native-dma-fault-test.cpp` covers create, partial create, page-table allocation, mmap, sync, bind-after-map, unbind invalidation, destroy failure, retry, and counter drain. The QEMU runner builds, copies, runs, and requires its PASS line. Kernel probe adds backing allocation/mapping and direction-rejection injection. |
| 17 | Evidence classification | **PASS** | QEMU happy-path and QEMU fault-injection buckets are now separate and populated. CPU-model evidence remains separate. Physical i915-assisted and independent physical buckets remain empty. |

---

## 3. Main code changes

### Kernel provider and UAPI

- `kernel/idw_edu_dma_test_main.c`
  - Split 39-bit streaming and 40-bit coherent masks.
  - `remove` waits for the opener, serializes teardown with `ops_lock`, and
    quarantines instead of freeing outstanding resources.
  - Resource and page-table VMAs now hold provider and module references.
  - `FREE`/`FREE_PT` reject live mappings; provider removal waits for
    `vma_refs` to drain.
  - Resource mappings use explicit write-combined protection.
  - Added sync-direction rejection and backing allocation/mapping fault
    injection to the probe self-test.
- `kernel/idw_dma_memory.c`
  - `idw_dma_provider_fini()` fails closed while any page, resource, or VMA is
    live.
  - Added provider ownership counters, `vma_refs`, `removal_blocked`, and
    stats.
  - Page-table pages now track mappings and reject free while mapped.
  - Resource sync validates direction compatibility and passes the requested
    direction to the DMA API.
  - Added test-only `fail_alloc_at` / `fail_map_at`.
- `include/uapi/idw_dma_test.h`
  - Added `struct idw_dma_test_sync`, `SYNC`/`SYNC_PT` ioctls, `stats`, and
    `fault` ioctls.

### Native backend

- `include/idw/native_dma_backend.hpp` / `src/native_dma_backend.cpp`
  - Added the checked `ResourceState` machine, `OwnershipCounters`, fault plan,
    quarantine, retry, and counter-drain assertions.
  - `quiesce_cb()` and `close_all()` prove inactivity instead of asserting it.
  - Bind failures after PPGTT publication are recoverable by unbind.
  - Destroy/unbind failures enter quarantine and require an explicit retry.

### Tests

- `tests/native-dma-fault-test.cpp` (new)
- `tests/native-dma-backend-test.cpp` (state, counter, stale-handle checks)
- `tests/qemu-dma/guest_init.c` (runs both test programs)
- `tests/qemu-dma/run.sh` (builds, copies, and requires both PASS lines)
- `Makefile` (`native-dma-fault-test`, `native-dma-fault-test-static`)

---

## 4. Verification

Commands run in this container:

```sh
make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build
make native-dma-backend-test native-dma-fault-test \
     native-dma-backend-test-static native-dma-fault-test-static \
     memory-manager-test ppgtt-model-test pt-dma-adapter-test abi-test
make stage3a-sanitize-test
KDIR=/lib/modules/6.12.117+deb13-amd64/build \
IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 \
make qemu-dma-test
```

Results:

- Kernel modules: PASS.
- Native/fault tests outside QEMU: SKIP because `/dev/idw-edu-dma` is absent.
- Memory manager, PPGTT model, adapter, ABI, and sanitizer tests: PASS.
- QEMU EDU TCG: PASS, `QEMU exit status: 33`.
- QEMU log includes:
  - `accepted 39-bit streaming and 40-bit coherent masks`
  - `PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup`
  - `PASS: native DMA backend fault injection, retry, and ownership counters`
  - `remove released all provider resources result=0`

Raw excerpts and hashes are in
`tests/qemu-dma/STAGE3C_REMEDIATION_EVIDENCE.md`.

---

## 5. Remaining limitation

Check 15 is still **INSUFFICIENT EVIDENCE** because this container has no
physical Intel GPU. The source contains no physical Haswell operation, but the
required read-only `lspci -nnk -s 00:02.0` and
`/sys/bus/pci/devices/0000:00:02.0/driver` evidence must be collected on a host
that exposes the GPU. This is an environment limitation, not a code defect.
