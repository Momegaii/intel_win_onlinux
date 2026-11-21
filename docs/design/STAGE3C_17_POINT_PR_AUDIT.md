# Stage 3C — Final 17-Point PR Audit

> **Historical audit.** Remediation is recorded in
> `docs/design/STAGE3C_REMEDIATION_REPORT.md` and
> `tests/qemu-dma/STAGE3C_REMEDIATION_EVIDENCE.md`. Check 15 remains
> environment-limited because this container exposes no physical Intel GPU.

**Repository:** `Momegaii/intel_win_onlinux`  
**Pull request:** [#5 — Add native DMA resource backing through MemoryManager](https://github.com/Momegaii/intel_win_onlinux/pull/5)  
**Target branch:** `master`  
**Audit date:** 2026-11-21 UTC  
**Audit mode:** Read-only implementation audit; documentation-only output  
**Final recommendation:** **REQUEST CHANGES**

---

## 1. Identification and preflight

| Item | Value |
|---|---|
| Repository HEAD at audit start | `fa2e91879b02526b4a31615ddf19561443d8e3cc` |
| PR base commit | `c596f5b8047287b10b8f5b22da9c0613746185bd` |
| PR commits | `b9f3dd6` (host provider), `fa2e918` (native DMA resource backing) |
| PR state | `closed`, `merged: true` at `2026-11-21T09:04:04Z` |
| Merge commit | `fa2e91879b02526b4a31615ddf19561443d8e3cc` |
| Branch | `master` |
| Shallow clone | `true` |
| Worktree at preflight | clean (`git status --porcelain=v1` produced no output, exit 0) |
| Audit output | Only the two requested audit files were created; no implementation, test, build, UAPI, or configuration file was edited |

The complete PR diff is `c596f5b..fa2e918`: 24 files, 3,690 insertions, 21 deletions. It includes the host-memory provider from `b9f3dd6` and the native DMA resource path from `fa2e918`. This audit focuses on the Stage 3C native DMA contract, while checking the full PR for boundary violations.

**Safety boundary:** No physical Haswell BAR, MMIO, DMA, GPU submission, PPGTT activation, or PCI ownership change was performed. The host container exposes no `0000:00:02.0` device and does not load i915, so host i915 binding could not be verified from this environment. QEMU EDU evidence is kept separate from physical Haswell evidence.

---

## 2. Environment and evidence basis

| Item | Observed value |
|---|---|
| Distribution | Debian GNU/Linux 13 (trixie), Debian 13.7 |
| Host kernel | `6.8.0-1055-gke` |
| Compiler | gcc/g++ 14.2.0 (Debian 14.2.0-19) |
| QEMU | 10.0.13 (Debian `1:10.0.13+ds-0+deb13u1`) |
| Kernel headers | `6.8.0-1055-gke`, `6.12.117+deb13-amd64` |
| QEMU guest kernel | `6.12.117+deb13-amd64` |
| PCI `0000:00:02.0` | **absent** from container sysfs |
| i915 module | **not loaded** in container |
| `/dev/dri` | **absent** |
| `lspci` | **not installed** |
| Visible PCI devices | Intel 8086:1237, 8086:7110, 8086:7113; gve; virtio-pci; nvme. No Intel GPU. |

Detailed commands, exit codes, raw excerpts, and SHA-256 hashes are in `tests/qemu-dma/STAGE3C_AUDIT_EVIDENCE.md`.

---

## 3. Executive result

| Status | Count |
|---|---:|
| PASS | 4 |
| FAIL | 12 |
| NOT IMPLEMENTED | 0 |
| INSUFFICIENT EVIDENCE | 1 |

The QEMU EDU happy path is real virtual-device DMA evidence and it passed. It does not satisfy the Stage 3C contract. The implementation has no required lifecycle state machine, no required ownership counters, no quarantine, a 40-bit streaming mask where the contract requires 39 bits, and user-mapping/provider-removal lifetime defects that can free backing while a VMA or model still references it. The host i915 binding requirement is also unverified in this environment.

**Decision:** **REQUEST CHANGES**. The virtual DMA plumbing is useful and the happy path is reproducible, but the safety and evidence requirements are not met.

---

## 4. Seventeen-point scorecard

### Check 1 — Resource state machine and transition validation

- **Status:** **FAIL**
- **Source File & Function:** `include/idw/memory_manager.hpp:101-106` (`BindingState`); `include/idw/native_dma_backend.hpp:44-68` (`Allocation`, `PageTable`, `Directory`); `src/memory_manager.cpp:127-229` (`bind`, `finish_unbind`, `unbind`); `src/native_dma_backend.cpp:207-277` (`bind_cb_impl`, `unbind_cb_impl`).
- **Exact Relevant Lines:** The only lifecycle enum is `Unbound`, `Bound`, `NeedsUnbind`, `NeedsInvalidation` at `include/idw/memory_manager.hpp:101-106`. `Allocation` has only `bool bound` at `include/idw/native_dma_backend.hpp:50`. There are no `NEW`, `VA_RESERVED`, `BACKING_ALLOCATED`, `DMA_MAPPED`, `PPGTT_POPULATED`, `PT_SYNCHRONIZED`, `READY_INACTIVE`, `DESTROYING`, or `RELEASED` states. Double bind is rejected at `src/memory_manager.cpp:139-140`; destroy-while-bound is rejected at `src/memory_manager.cpp:416-418`; stale handles are rejected by `find_resource()` at `src/memory_manager.cpp:80-89`.
- **Test & Output:** `make memory-manager-test` exit 0; `PASS: integrated memory allocation, VA, binding, submission, and recovery lifecycle`. `make native-dma-backend-test` exit 0 but **skipped**: `SKIP: /dev/idw-edu-dma is not present; run under the QEMU EDU harness`. QEMU native test covers only allocate, bind, duplicate bind (`-EBUSY`), destroy-while-bound (`-EBUSY`), unbind, destroy, close (`tests/native-dma-backend-test.cpp:61-114`). There is no test for the required full transition sequence or illegal transitions.
- **Untested Failure Path:** No test drives `NEW -> VA_RESERVED -> BACKING_ALLOCATED -> DMA_MAPPED -> PPGTT_POPULATED -> PT_SYNCHRONIZED -> READY_INACTIVE -> DESTROYING -> RELEASED`. No test covers a failed bind after PDE publication, an invalidation failure followed by retry, double destroy, stale handles after cookie reuse, or partial rollback from each state.
- **Blocking Defects and Required Corrections:** The required state machine is **not implemented**. Add an explicit resource state enum with checked transitions and terminal states, record the VA reservation, backing allocation, DMA mapping, PPGTT population, PT synchronization, inactive-ready, destroying, and released states, and reject every illegal transition. A failed partial bind must either roll back to the prior state or enter a quarantine state that cannot be silently cleaned.

### Check 2 — Independent ownership counters and leak assertions

- **Status:** **FAIL**
- **Source File & Function:** `include/idw/native_dma_backend.hpp:142-149` (backend members); `src/native_dma_backend.cpp:761-769` (`allocation_count`, `page_table_count`); `src/memory_manager.cpp:445-453` (`resource_count`, `submission_count`); `include/idw/dma_memory.h:54-86` (`idw_dma_resource`, provider); `src/pt_dma_adapter.c:71,86-87` (`outstanding`).
- **Exact Relevant Lines:** The backend has vectors `allocations_` and `page_tables_` but no `allocated_pages`, `mapped_pages`, `populated_ptes`, `pt_pages`, `va_reservations`, or `quarantined_pages` counters. `allocation_count()` returns `allocations_.size()` (`src/native_dma_backend.cpp:761-764`); `page_table_count()` returns `page_tables_.size()` (`:766-769`). The provider tracks `mappings` per resource at `include/idw/dma_memory.h:61` and page/resource lists at `:75-86`. `idw_pt_dma_adapter` has one `outstanding` count at `src/pt_dma_adapter.c:71,86-87`.
- **Test & Output:** QEMU native test checks `backend.allocation_count() == 1`, then `0` (`tests/native-dma-backend-test.cpp:45,105`), `manager.resource_count() == 0`, `manager.submission_count() == 0`, `backend.page_table_count() == 0` (`:109-114`). It does not check the six required counters. `make native-dma-backend-test` locally only printed the skip line, not a pass.
- **Untested Failure Path:** No test asserts that `populated_ptes` returns to zero after unmap, that `mapped_pages` and `allocated_pages` return to zero after teardown, that `va_reservations` returns to zero, or that `quarantined_pages` remains nonzero after an unsafe teardown. No counter is checked after a partial allocation or provider removal.
- **Blocking Defects and Required Corrections:** The required independent counters are **not implemented**, and the existing vector sizes are not leak assertions for PTEs, VA reservations, or quarantined resources. Add real counters updated at each acquisition/release point, assert zero on successful teardown, and keep quarantined resources accounted separately. Do not derive counters from unrelated vectors that can drift.

### Check 3 — Per-page DMA mapping and unmapping symmetry

- **Status:** **PASS** (source symmetry; failure injection not exercised)
- **Source File & Function:** `kernel/idw_dma_memory.c:218-313` (`idw_dma_resource_alloc`), `:58-74` (`idw_dma_resource_release_pages`), `:415-463` (`idw_dma_resource_free`), `:76-109` (`idw_dma_provider_fini`).
- **Exact Relevant Lines:** Each page is allocated at `:259`, mapped at `:270-271` with the stored direction, and stored at `:286-289`. A failed map is checked at `:272-276`. A mapped address that is misaligned or above the 39-bit resource limit is unmapped at `:280-282`. The partial-map cleanup loop unmaps every previously mapped page at `:303-308`. Normal release unmaps each page at `:65-70`. `idw_dma_resource_free()` calls release at `:456`.
- **Test & Output:** QEMU EDU happy path exit 0, QEMU status 33. Raw log includes `IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified` and `IDW_QEMU_DMA: 32 device DMA read/write cycles verified`. No fault-injection run exists for a first/middle/last `dma_map_page()` failure.
- **Untested Failure Path:** Partial-map failure after the first or middle page, `dma_mapping_error()` injection, and provider teardown while pages are mapped are not exercised. The source path is symmetric, but it is not proven by a failing-map test.
- **Blocking Defects and Required Corrections:** No mapping/unmapping symmetry defect was found in the inspected source. Add injectable `dma_map_page()` failure at first/middle/last page and verify every successful map has exactly one unmap with the same page, size, direction, and device.

### Check 4 — DMA mask configuration and address limits

- **Status:** **FAIL**
- **Source File & Function:** `kernel/idw_edu_dma_test_main.c:986-999` (`idw_edu_probe` mask setup); `include/idw/dma_memory.h:14-23` (address bits/masks); `kernel/idw_dma_memory.c:33-35` (provider mask check), `:138-151` (coherent page check), `:270-285` (resource map check); `src/haswell_ppgtt_model.c:52-68` (DMA limits); `include/idw/haswell_ppgtt_format.h:32-33,40-76` (encoders).
- **Exact Relevant Lines:** The driver calls `dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))` at `:986`, then `dma_set_mask_and_coherent(&pdev->dev, IDW_DMA_ADDRESS_MASK)` at `:996`. `IDW_DMA_ADDRESS_MASK` is `DMA_BIT_MASK(40)` at `include/idw/dma_memory.h:14-15`. There is no separate 39-bit streaming mask. Resource addresses are checked after mapping at `kernel/idw_dma_memory.c:277-285` against `IDW_DMA_RESOURCE_ADDRESS_MASK` (`2^39`), and page-table addresses are checked at `:144-150` against the 40-bit mask. The model independently rejects page addresses `>= 2^39` and PT addresses `>= 2^40` at `src/haswell_ppgtt_model.c:52-68`.
- **Test & Output:** QEMU log: `IDW_QEMU_DMA: rejected 32-bit mask; accepted configured 40-bit mask`. The test validates that resource DMA addresses are below `2^39` (`kernel/idw_edu_dma_test_main.c:422-437`) and page-table addresses are below `2^40` (`:221-239`). It does **not** configure a 39-bit streaming mask.
- **Untested Failure Path:** No test forces the allocator to return a resource DMA address at or above `2^39`; the post-map check is not exercised with an out-of-range address. No test verifies a 39-bit `dma_set_mask()` result.
- **Blocking Defects and Required Corrections:** The contract requires a **39-bit streaming resource DMA mask** and a **40-bit coherent page-table DMA mask**. The code configures **40 bits for both** and only post-checks resource addresses. This is a contract failure. Configure the streaming mask separately with `dma_set_mask(dev, DMA_BIT_MASK(39))`, keep the coherent mask at 40 bits, require both before allocation, and test a rejected 40-bit streaming request. Update `tests/qemu-dma/README.md:11`, which currently says “sets 40-bit streaming/coherent DMA masks.”

### Check 5 — DMA synchronization and mapping direction

- **Status:** **FAIL** (basic BIDIRECTIONAL path passes; API direction contract is unsafe)
- **Source File & Function:** `kernel/idw_dma_memory.c:353-413` (`idw_dma_resource_sync`); `kernel/idw_edu_dma_test_main.c:905-922` (`IDW_DMA_TEST_IOCTL_SYNC`); `src/haswell_ppgtt_model.c:185-186,498,563` (zero/publish/clear sync); `src/native_dma_backend.cpp:583-594` (`sync_page_table_for_device`).
- **Exact Relevant Lines:** Resource sync takes a caller-supplied `direction` at `:353-367`, but when it calls the DMA API it passes `resource->direction`, not the requested direction, at `:394-403`. The ioctl accepts any of the three directions at `kernel/idw_edu_dma_test_main.c:914-916` and does not compare it with the resource's mapping direction. Page-table allocation zeroes and syncs at `src/haswell_ppgtt_model.c:185-186`; PTE publication syncs at `:495-503`; PTE clearing syncs at `:551-568`.
- **Test & Output:** QEMU test uses `DMA_TO_DEVICE` before the device reads source/destination and `DMA_FROM_DEVICE` after the device writes destination (`kernel/idw_edu_dma_test_main.c:477-495`). The resources are mapped `DMA_BIDIRECTIONAL` (`:454-459`). The QEMU log reports `sync, and cleanup verified` and exit status 33. There is no test of an incompatible requested direction.
- **Untested Failure Path:** No test passes `DMA_FROM_DEVICE` for a `DMA_TO_DEVICE` mapping or the reverse; no test injects a sync failure; no test checks the API rejects a direction that is invalid for the stored mapping.
- **Blocking Defects and Required Corrections:** The sync implementation can call `dma_sync_single_for_cpu()` or `dma_sync_single_for_device()` with a direction different from the original mapping, which violates the DMA API direction contract. Reject incompatible directions at the ioctl/provider boundary or store and require the mapping direction. Add tests for all direction combinations and for sync failure.

### Check 6 — Safe userspace mapping

- **Status:** **FAIL**
- **Source File & Function:** `kernel/idw_edu_dma_test_main.c:645-655` (`idw_edu_resource_vm_close`), `:657-679` (`idw_edu_open`/`idw_edu_release`), `:681-731` (`idw_edu_mmap`), `:885-903` (`FREE_PT`); `kernel/idw_dma_memory.c:76-109` (`idw_dma_provider_fini`), `:415-463` (`idw_dma_resource_free`).
- **Exact Relevant Lines:** Resource mmap validates cookie and exact length at `:706-716`, sets `vm_ops` and `vm_private_data` at `:717-720`, inserts pages at `:721-726`, and sets `resource->mappings = 1` at `:728`. Page-table mmap validates cookie and exactly one page at `:691-699`, then calls `dma_mmap_coherent()` at `:701-703`; it has **no mapping count and no vm_ops**. `idw_edu_release()` only clears `open` and completes `closed` at `:670-679`; it does not zap VMAs. `FREE_PT` frees the coherent page at `:885-903` without checking whether it is mapped. `idw_dma_provider_fini()` frees coherent pages and resource pages at `:92-103` without checking VMA references. Resource mmap uses `vm_insert_page()` with no explicit `pgprot`/cache attribute at `:721-726`.
- **Test & Output:** QEMU native test maps the resource and then unmaps through backend teardown; it passes. No test mmaps a resource or page table, closes the fd while keeping the VMA, then invokes `FREE`, `FREE_PT`, or module removal. No test races `mmap` against `FREE`.
- **Untested Failure Path:** VMA outliving the fd; VMA outliving the provider; `FREE_PT` while the PT VMA is mapped; concurrent `mmap`/`FREE`; failed partial `vm_insert_page()`; read-only or private mapping; cache-attribute behavior.
- **Blocking Defects and Required Corrections:** A page-table VMA is completely untracked, so `FREE_PT` can free a coherent page still mapped to userspace. A resource VMA can outlive the fd because `release()` does not zap VMAs, and provider removal only waits for `test->open`, not for VMAs. `resource->mappings` is a non-atomic count checked without a common lock. These are use-after-free/lifetime defects. Add a VMA reference/lifetime object, track both resource and PT mappings, reject free/remove while mapped, zap VMAs on release/remove, and serialize `mmap`/`FREE`/`FREE_PT` with the provider lock. Set explicit cache attributes or document the default mapping policy.

### Check 7 — Existing Haswell PPGTT encoder reuse and DMA validation

- **Status:** **PASS**
- **Source File & Function:** `src/native_dma_backend.cpp:229-238` (`bind_cb_impl` calls model map), `:481-550` (`alloc_page_table_page`); `src/haswell_ppgtt_model.c:458-488` (PDE/PTE encode), `:52-68` (address limits); `include/idw/haswell_ppgtt_format.h:40-76` (separate PDE/PTE encoders); `kernel/idw_dma_memory.c:270-285`.
- **Exact Relevant Lines:** Native bind calls `idw_hsw_ppgtt_map(model_, requested_va, bytes, mappings.data(), mappings.size())` at `src/native_dma_backend.cpp:232-233`. The model calls `idw_hsw_ppgtt_encode_pde()` at `:458-459` and `idw_hsw_ppgtt_encode_pte(..., IDW_HSW_PPGTT_PTE_CACHE_UC, ...)` at `:486-488`. The encoders use the separate PDE mask `0x00000ff0` and PTE mask `0x000007f0` at `include/idw/haswell_ppgtt_format.h:49-51,71-74`. Resource page addresses are limited to `< 2^39` by `page_dma_valid()` at `src/haswell_ppgtt_model.c:61-68` and by the kernel post-map check at `kernel/idw_dma_memory.c:277-285`; PT addresses are limited to `< 2^40` at `src/native_dma_backend.cpp:497-500` and `src/haswell_ppgtt_model.c:52-59`.
- **Test & Output:** `make ppgtt-model-test` exit 0: `PASS: CPU-only GPU-VA allocator and Haswell Gen7.5 PPGTT model tests`; it includes boundary vectors for `< 2^39` PTE and `< 2^40` PDE (`tests/ppgtt-model-test.c:312-324`). QEMU native test compares `backend.translate()` with the real resource DMA address at `tests/native-dma-backend-test.cpp:65-79`; QEMU log reports `PPGTT PTE consumption ... verified`, exit 33.
- **Untested Failure Path:** No physical Haswell page walk. No test with a real page-table DMA address in the 39–40 bit range; the QEMU page table is below 4 GiB. No test of a resource address exactly at `2^39` or a PT address exactly at `2^40` through the native backend.
- **Blocking Defects and Required Corrections:** No encoder duplication or width defect was found. Preserve the existing encoders and add boundary tests through the native backend, not only the CPU model.

### Check 8 — Explicit cacheability policy

- **Status:** **FAIL**
- **Source File & Function:** `src/haswell_ppgtt_model.c:486-488` (PTE cacheability); `include/idw/haswell_ppgtt_format.h:38` (`IDW_HSW_PPGTT_PTE_CACHE_UC`); `kernel/idw_edu_dma_test_main.c:701-703` (PT mmap), `:721-726` (resource mmap); `docs/design/NATIVE_DMA_RESOURCE_BACKING_IMPLEMENTATION.md:113-131`; `docs/design/HASWELL_GEN75_PHYSICAL_PPGTT_READINESS.md:144`.
- **Exact Relevant Lines:** The PTE is explicitly encoded UC at `src/haswell_ppgtt_model.c:486-488` and the constant is `0x0` at `include/idw/haswell_ppgtt_format.h:38`. The resource VMA uses `vm_insert_page()` with the VMA's default protection at `kernel/idw_edu_dma_test_main.c:721-726`; no `pgprot_noncached`, `pgprot_writecombine`, or DMA attribute is set. The page-table VMA uses `dma_mmap_coherent()` at `:701-703` without an explicit attribute argument. The design document states cache-policy programming remains future work at `docs/design/NATIVE_DMA_RESOURCE_BACKING_IMPLEMENTATION.md:129-131`; the readiness document says global PPGTT cache policy is unchanged and a physical blocker at `docs/design/HASWELL_GEN75_PHYSICAL_PPGTT_READINESS.md:144`.
- **Test & Output:** QEMU only. It verifies the PTE cache bit vector in the model (`tests/ppgtt-model-test.c:323-334`) and the kernel self-test checks `0x00000801` for cacheability `0x8` (`kernel/idw_edu_dma_test_main.c:343-351`). It does not validate CPU mapping cache attributes or Haswell global cache policy.
- **Untested Failure Path:** No physical Haswell cache-policy validation; no test of resource VMA cache attributes; no test of `ECOCHK`/`GAC_ECO_BITS`; no coherency test with a non-snooped device.
- **Blocking Defects and Required Corrections:** The PTE UC choice is explicit, but the resource userspace mapping policy is implicit and the global Haswell policy is absent. Add explicit cache attributes for the VMA and document the software-only assumption. Do not claim silicon validation. If the contract requires explicit resource and page-table policy in both code and documentation, this check cannot pass until that policy is defined and tested.

### Check 9 — PPGTT model detachment before PT backing release

- **Status:** **FAIL** (normal close path is ordered; removal/lifetime path is not)
- **Source File & Function:** `src/native_dma_backend.cpp:687-705` (`close_all`); `src/haswell_ppgtt_model.c:352-371` (`destroy_checked`), `:198-206` (`free_table`); `kernel/idw_edu_dma_test_main.c:1080-1117` (`idw_edu_remove`); `kernel/idw_dma_memory.c:76-109` (`idw_dma_provider_fini`).
- **Exact Relevant Lines:** `close_all()` refuses while allocations remain (`:689-690`), destroys the model with `idw_hsw_ppgtt_model_destroy_checked(model_, true)` (`:691-696`), then checks `page_tables_.empty()` (`:697-698`). `destroy_checked()` frees every table and the directory at `src/haswell_ppgtt_model.c:362-369`. However, `idw_edu_remove()` only waits for `test->open` at `kernel/idw_edu_dma_test_main.c:1092-1093`; a VMA can remain after the fd is closed. `idw_dma_provider_fini()` then frees all page-table coherent pages and resources at `kernel/idw_dma_memory.c:92-103`.
- **Test & Output:** QEMU native test calls `manager.destroy`, `manager.unbind`, `manager.destroy`, `manager.shutdown`, and `backend.close_all()` in order (`tests/native-dma-backend-test.cpp:98-114`), then the module is removed (`tests/qemu-dma/guest_init.c:127-135`). QEMU log: `remove freed outstanding provider allocations=yes result=0`. This proves the happy path, not the outstanding-model path.
- **Untested Failure Path:** Close the fd while retaining a resource or page-table VMA; leave the userspace model alive while removing the provider; fail `FREE_PT` during model destroy; fail `idw_hsw_ppgtt_model_destroy_checked`; remove the module with a live backend object.
- **Blocking Defects and Required Corrections:** The kernel provider can free page-table backing while a userspace `idw_hsw_ppgtt_model` still references it, because model lifetime is not tied to the fd/VMA/provider. This violates the required ordering. Add a kernel-visible model/backing reference, require explicit backend close before remove, and quarantine page-table pages if detachment cannot be proven.

### Check 10 — Reverse-order rollback and quarantine

- **Status:** **FAIL**
- **Source File & Function:** `src/memory_manager.cpp:183-229` (`finish_unbind`, `unbind`), `:407-425` (`destroy`); `src/native_dma_backend.cpp:249-277` (`unbind_cb_impl`), `:687-705` (`close_all`); `src/haswell_ppgtt_model.c:499-518` (sync failure/rollback), `:648-657` (`invalidation_failed`); `kernel/idw_dma_memory.c:76-109`.
- **Exact Relevant Lines:** `unbind_cb_impl()` clears PTEs with `idw_hsw_ppgtt_unmap()` at `src/native_dma_backend.cpp:260`, then attempts `idw_hsw_ppgtt_invalidation_complete()` at `:268-272`, and only then marks the allocation unbound at `:274-275`. If invalidation completion fails, `MemoryManager::unbind()` leaves the state `NeedsUnbind` (`src/memory_manager.cpp:226-228`). A retry calls `idw_hsw_ppgtt_unmap()` again; the PTEs are already zero, so `idw_hsw_ppgtt_unmap()` returns `-ENOENT` at `src/haswell_ppgtt_model.c:546-549`. There is no quarantine state or counter. `close_all()` destroys the model with `true` at `src/native_dma_backend.cpp:691-696`; provider fini frees all pages at `kernel/idw_dma_memory.c:92-103`.
- **Test & Output:** `make ppgtt-model-test` exit 0 includes simulated invalidation failure and page-table sync failure (`tests/ppgtt-model-test.c:517-545,790-805`), but those tests exercise the CPU model, not the native backend/kernel lifetime. `make pt-dma-adapter-test` exit 0 covers simulated adapter rollback (`tests/pt-dma-adapter-test.c:390-418,536-543`). No QEMU native test injects a native unbind/invalidation failure or a provider-removal rollback.
- **Untested Failure Path:** PTE clear success followed by invalidation failure; retry; partial PDE publication failure; PT allocation failure after some tables are staged; sync failure after PTE writes; provider removal with outstanding resources; rollback failure itself.
- **Blocking Defects and Required Corrections:** There is no quarantine implementation, and the retry path after invalidation failure is broken because the PTEs are already cleared. `close_all()` asserts hardware quiescence instead of proving it. Implement a quarantine state for any resource whose PTE clear/sync/invalidation cannot be proven, prevent cleanup/provider removal from freeing it, and add failure-injection tests for each reverse-order step.

### Check 11 — UAPI permissions and device lifecycle safety

- **Status:** **FAIL**
- **Source File & Function:** `kernel/idw_edu_dma_test_main.c:657-679` (`open`/`release`), `:681-731` (`mmap`), `:733-943` (`ioctl`), `:1027-1034` (misc device mode); `include/uapi/idw_dma_test.h:8-15`; `kernel/idw_edu_dma_test_main.c:1119-1123` (EDU-only ID table).
- **Exact Relevant Lines:** The misc device is created with `mode = 0600` at `:1027-1034`. `open` uses `atomic_cmpxchg` to allow one opener at a time at `:663-664`. The UAPI header says test-only and not stable at `include/uapi/idw_dma_test.h:8-15`; the driver binds only `PCI_DEVICE(0x1234, 0x11e8)` at `kernel/idw_edu_dma_test_main.c:1119-1123`, so it is not a production Intel GPU ownership path. However, `release()` does not zap VMAs (`:670-679`), `mmap` and `FREE`/`FREE_PT` are not serialized by a common lock, and `resource->mappings` is a plain `u32` (`include/idw/dma_memory.h:61`) updated at `kernel/idw_edu_dma_test_main.c:649-650,715-716,728`.
- **Test & Output:** QEMU test opens the device once, runs the backend, closes, and unloads the module. No test attempts a second open, concurrent ioctl/mmap, stale cookie, close-with-VMA, or `FREE_PT` while mapped.
- **Untested Failure Path:** Concurrent `mmap`/`FREE`; close fd with live VMA; `FREE_PT` with live PT VMA; second opener while first has a VMA; stale cookie after free; failed `copy_to_user` after allocation; module removal with an in-flight ioctl.
- **Blocking Defects and Required Corrections:** The test-only permission boundary is acceptable, but the lifecycle is not safe. Add a per-device mutex covering lookup/mmap/free/remove, an explicit VMA reference count, `zap_vma_ptes()`/VMA teardown on release and remove, and reject `FREE`/`FREE_PT` while mapped. Ensure module removal waits for all VMAs and ioctls, not only the open count.

### Check 12 — Provider removal with outstanding allocations

- **Status:** **FAIL**
- **Source File & Function:** `kernel/idw_edu_dma_test_main.c:1080-1117` (`idw_edu_remove`); `kernel/idw_dma_memory.c:76-109` (`idw_dma_provider_fini`), `:415-463` (`idw_dma_resource_free`).
- **Exact Relevant Lines:** `idw_edu_remove()` computes `outstanding` from the provider lists at `:1100-1101`, calls `idw_dma_provider_fini()` at `:1102`, and reports success when `outstanding && !ret` at `:1113-1116`. `idw_dma_provider_fini()` frees all coherent pages at `:92-97` and all resource pages at `:98-103` without checking `resource->mappings` or any VMA. The normal `idw_dma_resource_free()` does check `entry->mappings` at `:451-454`, but provider fini bypasses that check.
- **Test & Output:** QEMU log explicitly reports `IDW_QEMU_DMA: remove freed outstanding provider allocations=yes result=0`. The test treats freeing outstanding allocations as success. That is evidence of the behavior, not evidence of safety.
- **Untested Failure Path:** Removal with a live resource VMA; removal with a live page-table VMA; removal while a model still references PT pages; removal while an ioctl is in flight; `dma_free_coherent()` failure (not representable).
- **Blocking Defects and Required Corrections:** Provider removal frees pages that may still be mapped or referenced, directly violating the check. It must refuse removal until all VMAs, ioctls, and models are detached, or quarantine the pages and report failure. Do not report `result=0` for an outstanding-allocation removal unless the pages were proven unreferenced.

### Check 13 — Inactive-hardware invariant

- **Status:** **FAIL**
- **Source File & Function:** `src/native_dma_backend.cpp:352-357` (`quiesce_cb`), `:687-696` (`close_all`); `src/haswell_ppgtt_model.c:352-371` (`destroy_checked`), `:670-685` (`set_active`/`is_active`); `include/idw/memory_manager.hpp:48-49` (quiesce contract).
- **Exact Relevant Lines:** `quiesce_cb()` ignores both arguments and returns `0` unconditionally at `:352-357`. `close_all()` calls `idw_hsw_ppgtt_model_destroy_checked(model_, true)` with a literal `true` at `:691-696`. `destroy_checked()` only checks `active`/`invalidation_pending` when `hardware_quiesced` is false at `src/haswell_ppgtt_model.c:359-361`; the literal `true` bypasses that check. The model has `active` and `invalidation_pending` fields at `:35-37`, but the native backend never sets or queries them (`grep` found no `idw_hsw_ppgtt_set_active`/`is_active` call in the native path).
- **Test & Output:** QEMU native test calls `close_all()` and receives success (`tests/native-dma-backend-test.cpp:112-114`). There is no test that the backend establishes no hardware activation, no live page-table publication, no hardware submission, no outstanding invalidation, and no uncertain device access. The QEMU EDU device is not a Haswell GPU.
- **Untested Failure Path:** Active model; pending invalidation; quiesce with a bad fence; provider removal during uncertain access; a backend capability that cannot prove inactivity.
- **Blocking Defects and Required Corrections:** `READY_INACTIVE` is not implemented as a trusted state/capability. A caller-supplied/ hardcoded boolean (`true`) and an unconditional `quiesce` success are not proof. Add an explicit backend capability/state that records no activation, no live publication, no submission, and no pending invalidation, and fail closed if any cannot be proven. Remove the literal quiescence assertion.

### Check 14 — PPGTT evidence separation

- **Status:** **PASS** (documentation separation; physical bucket empty)
- **Source File & Function:** `docs/design/NATIVE_DMA_RESOURCE_BACKING_IMPLEMENTATION.md:3-5,113-131`; `tests/qemu-dma/README.md:3-19,58-59`; `docs/IMPLEMENTATION_STATUS.md:130-132`.
- **Exact Relevant Lines:** The design document states “No physical Haswell operation, MMIO access, BAR mapping, i915 unbind, or PCI ownership change was performed” at `:3-5`, describes the CPU model and simulated page walk at `:113-121`, and lists “no MMIO, forcewake, engine activation, reset, or physical Haswell DMA” at `:121-122`. The QEMU README says the EDU test is “device-side DMA in QEMU, not ... proof of Haswell DMA behavior” at `:3-7` and “does not bind, unbind, or otherwise alter the host's physical GPU” at `:58-59`.
- **Test & Output:** QEMU log identifies `QEMU version: 10.0.13`, `Acceleration: TCG (KVM not used)`, `Device: EDU PCI 1234:11e8, dma_mask=0xffffffffff`; it reports virtual DMA pass only. No physical GPU test exists.
- **Untested Failure Path:** Physical Haswell page walk; physical PPGTT activation; physical cache policy; physical DMA; i915-assisted physical execution.
- **Blocking Defects and Required Corrections:** No separation defect was found. Keep the five buckets distinct; do not promote QEMU DMA to physical Haswell evidence. The `i915-assisted physical execution` and `independent physical GPU` buckets are empty.

### Check 15 — GGTT and hardware execution boundary

- **Status:** **INSUFFICIENT EVIDENCE** (code boundary appears clean; host i915 binding unverified)
- **2026-11-21 follow-up:** added `tests/physical/haswell-evidence.sh` and
  `make physical-evidence`. The current Google Compute Engine VM returns
  `TARGET_GPU_NOT_PRESENT`; the target device, `/dev/dri`, i915, IOMMU groups,
  and `/dev/vfio` are absent. Check 15 remains open.
- **Source File & Function:** `kernel/idw_edu_dma_test_main.c:25-37` (EDU IDs/registers), `:973` (EDU BAR `pci_iomap`), `:61` (EDU `readl_poll_timeout`), `:1119-1123` (EDU-only ID table); `src/native_dma_backend.cpp:687-705` (no MMIO); `src/haswell_ppgtt_model.c:486-488` (CPU model PTE writes).
- **Exact Relevant Lines:** The only `pci_iomap`/`readl`/`writeq` operations in the native test path target the QEMU EDU BAR (`kernel/idw_edu_dma_test_main.c:61,69-74,973`). The driver ID table is `PCI_DEVICE(0x1234, 0x11e8)` at `:1119-1120`. No native backend code references a Haswell PCI ID, GPU BAR, MMIO register, ring, GGTT, or `PP_DIR_BASE`. The model writes PTEs to a userspace page-table page at `src/haswell_ppgtt_model.c:486-491`, not to hardware.
- **Test & Output:** `lspci` is not installed. `/sys/bus/pci/devices/0000:00:02.0` is **absent**; `grep -w i915 /proc/modules` is **absent**; `/dev/dri` is **absent**. Visible PCI devices are 8086:1237, 8086:7110, 8086:7113, gve, virtio-pci, and nvme. Therefore this audit cannot verify that `0000:00:02.0` remains bound to i915. It also found no evidence of a physical operation being attempted.
- **Untested Failure Path:** Host i915 binding; physical BAR ownership; physical MMIO; physical GGTT; physical ring submission; physical PPGTT activation.
- **Blocking Defects and Required Corrections:** No physical hardware operation was found in the inspected source, and no physical operation was performed by this audit. However, the required i915 binding evidence is missing. Re-run the read-only `lspci -nnk -s 00:02.0` and `/sys/bus/pci/devices/0000:00:02.0/driver` checks on a host that exposes the GPU, and record the result. Do not claim binding from this container.

### Check 16 — Failure-path evidence

- **Status:** **FAIL** (full matrix still missing; D2 adds probe-failure coverage only)
- **D2 follow-up (2026-11-21):** `tests/qemu-dma/run.sh --probe-failure` now
  injects the real `fail_provider` path, exercises registry reachability,
  deferred PCI cleanup, and two recovery rounds. The remaining allocation,
  mapping, sync, mmap, quiescence, and full removal failure matrix is still
  required; this Check remains FAIL.
- **Source File & Function:** `tests/native-dma-backend-test.cpp:20-118` (native test); `tests/qemu-dma/run.sh:112-119` (QEMU success grep); `tests/qemu-dma/guest_init.c:127-135` (guest sequence); `tests/ppgtt-model-test.c:35-60,443-545,790-805` (model injection); `tests/pt-dma-adapter-test.c:34-42,390-418,536-543` (adapter injection); `kernel/idw_edu_dma_test_main.c:221-283` (kernel malformed descriptors).
- **Exact Relevant Lines:** The native test covers only the happy lifecycle plus duplicate bind and destroy-while-bound (`tests/native-dma-backend-test.cpp:61-63,98-104`). The QEMU runner only checks four success strings and exit 33 (`tests/qemu-dma/run.sh:112-119`); it has no fault-injection mode. The kernel self-test rejects malformed PT descriptors, wrong scope, wrong length, wrong cookie, and unsupported mapping (`kernel/idw_edu_dma_test_main.c:221-283`), and rejects invalid resource length/scope/cookie (`:549-574`). The model and adapter tests have simulated allocation, sync, write, read, and flush failures.
- **Test & Output:** `make ppgtt-model-test` and `make pt-dma-adapter-test` pass with simulated failures. `make native-dma-backend-test` locally skips. QEMU happy path passes. There is no QEMU fault-injection target or mode; `grep` of `tests/qemu-dma/run.sh`/`guest_init.c` found no fault-injection harness.
- **Untested Failure Path:** First, middle, and last backing-page allocation; first and middle DMA mapping; `dma_mapping_error()`; misaligned DMA address; out-of-range DMA address; page-table allocation failure; PTE population failure; page-table synchronization failure; userspace mapping failure; provider removal with outstanding resources; quiescence/invalidation uncertainty; rollback and quarantine.
- **Blocking Defects and Required Corrections:** The required failure matrix is mostly **not implemented** at the native/kernel boundary. Add injectable failures at each allocation/map/sync/mmap/removal step, assert the ownership counters after each, and add a QEMU fault-injection mode. Existing simulated model/adapter tests do not substitute for native-path failure evidence.

### Check 17 — Evidence classification

- **Status:** **PASS** (classification is explicit; two buckets are empty)
- **Source File & Function:** This report and `tests/qemu-dma/STAGE3C_AUDIT_EVIDENCE.md`.
- **Exact Relevant Lines:** Section 6 of this report and the evidence ledger maintain the five buckets:
  1. QEMU happy-path evidence: present, QEMU 10.0.13 TCG, exit 33.
  2. QEMU fault-injection evidence: **partial**; the D2 probe-failure mode
     exists, but the full failure matrix is still empty.
  3. CPU-model/software evidence: present, `ppgtt-model-test`, `pt-dma-adapter-test`, `memory-manager-test`, host backend.
  4. i915-assisted physical execution evidence: **empty**; i915 tests are simulated and no GPU is exposed.
  5. Independent physical GPU validation evidence: **empty**; no physical Haswell test.
- **Test & Output:** The evidence ledger records the exact commands, exit codes, raw excerpts, and SHA-256 hashes. QEMU is explicitly labeled virtual TCG; no physical claim is made.
- **Untested Failure Path:** Buckets 2, 4, and 5 have no evidence, by design or environment limitation.
- **Blocking Defects and Required Corrections:** No classification defect was found. Do not merge the empty buckets into a generic PASS.

---

## 5. Blocking defects

1. **Unsafe provider removal / VMA lifetime (critical).** `idw_dma_provider_fini()` frees coherent and streaming pages without checking VMA references (`kernel/idw_dma_memory.c:92-103`). `idw_edu_remove()` waits only for `test->open`, not VMAs (`kernel/idw_edu_dma_test_main.c:1089-1094`). A VMA can outlive the fd because `release()` does not zap it (`:670-679`). Page-table VMAs are not tracked at all (`:691-703`), and `FREE_PT` does not check them (`:885-903`).
2. **No quarantine and broken retry after invalidation failure.** `unbind_cb_impl()` clears PTEs before invalidation (`src/native_dma_backend.cpp:260-272`); if invalidation fails, a retry cannot clear them again (`src/haswell_ppgtt_model.c:546-549`). There is no quarantine counter/state; provider removal frees the resource anyway.
3. **Streaming DMA mask is 40 bits, not 39.** `dma_set_mask_and_coherent(..., IDW_DMA_ADDRESS_MASK)` sets both masks to 40 bits (`kernel/idw_edu_dma_test_main.c:996`; `include/idw/dma_memory.h:14-15`). The resource post-check does not satisfy the mask contract.
4. **No required lifecycle state machine.** Only `Unbound/Bound/NeedsUnbind/NeedsInvalidation` exists (`include/idw/memory_manager.hpp:101-106`); the required `NEW` through `RELEASED` states and transition validation are absent.
5. **No required ownership counters.** `allocated_pages`, `mapped_pages`, `populated_ptes`, `pt_pages`, `va_reservations`, and `quarantined_pages` do not exist; vector sizes and one adapter `outstanding` count are not substitutes.
6. **`READY_INACTIVE` is a literal boolean.** `close_all()` passes `true` to `destroy_checked()` (`src/native_dma_backend.cpp:691-696`), and `quiesce_cb()` always returns 0 (`:352-357`). There is no trusted inactive-hardware capability.
7. **Unsafe sync direction contract.** `idw_dma_resource_sync()` accepts any requested direction but passes `resource->direction` to the DMA API (`kernel/idw_dma_memory.c:394-403`), so incompatible requests are not rejected.
8. **Missing failure-path evidence.** No native/kernel fault injection for allocation, mapping, PT allocation, PTE population, sync, mmap, removal, quiescence, rollback, or quarantine.
9. **Host i915 binding unverified.** This container has no `0000:00:02.0`, no i915 module, and no `/dev/dri`; `lspci` is absent. No physical operation was attempted, but the binding claim cannot be proven here.

---

## 6. Test and validation matrix

| Command | Exit | Result |
|---|---:|---|
| `git status --porcelain=v1` | 0 | empty output; clean tree |
| `make memory-manager-test` | 0 | PASS; simulated backend |
| `make host-memory-backend-test` | 0 | PASS; real host pages |
| `make native-dma-backend-test` | 0 | **SKIP**: `/dev/idw-edu-dma` absent; not a pass |
| `make ppgtt-model-test` | 0 | PASS; CPU model only |
| `make pt-dma-adapter-test` | 0 | PASS; simulated provider |
| `make stage3a-test` | 0 | PASS; three tests |
| `make stage3a-sanitize-test` | 0 | PASS; ASan/UBSan on model/GGTT/lifecycle |
| `make kernel KDIR=/lib/modules/6.8.0-1055-gke/build` | 2 | FAIL: `gcc-13: not found` in header tree |
| `make kernel KDIR=/lib/modules/6.8.0-1055-gke/build CC=gcc` | 0 | PASS |
| `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build` | 0 | PASS |
| `make test` | 2 | 12 PASS, 1 SKIP; stops at `tests/smoke.sh`: no Intel render node |
| `KDIR=...6.12... IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 make qemu-dma-test` | 0 | PASS; QEMU exit status 33 |
| QEMU fault-injection | N/A | **Not available**; no target/mode |
| Native backend sanitizer | N/A | **Not available**; no target |

The QEMU pass is virtual-device evidence only. The local `native-dma-backend-test` skip is not a pass.

---

## 7. Physical Haswell validation status

**No physical Haswell validation exists in this audit.** No physical BAR, MMIO, DMA, GPU submission, GGTT publication, PPGTT activation, or PCI ownership change was performed. The QEMU EDU device is a virtual PCI device (`1234:11e8`); its DMA engine and the CPU PPGTT model prove address plumbing and software encoding only.

The host container does not expose `0000:00:02.0`; it does not load i915; it has no `/dev/dri`; and `lspci` is not installed. Therefore this audit cannot state that `0000:00:02.0` remained bound to i915. The correct status is **unverified in this environment**, not “bound” and not “unbound.”

---

## 8. Final recommendation

**REQUEST CHANGES.**

The PR provides a working QEMU EDU happy path and reuses the existing Haswell Gen7.5 encoders correctly. It does not satisfy Stage 3C because the required lifecycle/counter/quarantine contract is absent, the streaming mask is wrong, the sync direction contract is unsafe, provider removal and userspace mapping can free referenced backing, and the required failure-path and physical-binding evidence is missing.

Approval would require, at minimum:

1. Implement the explicit `NEW` through `RELEASED` state machine with checked transitions.
2. Implement the six independent ownership counters and zero/quarantine assertions.
3. Configure a 39-bit streaming mask and a 40-bit coherent mask separately.
4. Fix VMA/fd/provider lifetime, PT VMA tracking, locking, and removal semantics.
5. Implement quarantine and correct retry after PTE clear/invalidation failure.
6. Replace the literal quiescence boolean and unconditional `quiesce` success with a trusted inactive-hardware state/capability.
7. Add native/kernel fault injection for every required failure path.
8. Provide read-only host evidence that `0000:00:02.0` remains bound to i915.
