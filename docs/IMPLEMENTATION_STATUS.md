# Implementation status

Status labels: **working** = exercised by the current project; **scaffold** = source/design exists but not a complete subsystem; **research** = requires hardware/ABI research; **not implemented** = no functional implementation.

**Hardware validation state:** On 2026-11-20, the explicit i915-assisted
submission, memory-advice, and integrated manager tests passed on Haswell
`8086:0412` (`/dev/dri/renderD128`, driver i915). `gpu-test` completed three
no-op render submissions with successful `GEM_WAIT`; `gpu-memory-test` bound
at `0x10000000` / `0x10100000`, completed initial and rebound submissions, and
observed no purge. `gpu-memory-manager-test` completed three manager-tracked
allocation/bind/submit/wait/unbind/destroy lifecycles at `0x10000000`,
`0x10001000`, and `0x10002000`. These results confirm i915-assisted execution
and lifecycle only; no independent IDW GPU control is claimed. Passing
software checks is evidence about software behavior, not independent GPU
control.

**Stage 3A software infrastructure:** a checked GGTT allocator with residency,
pin, binding, and teardown state; a Haswell GTT hardware abstraction for
GTTADDR, GGTT flush, TLB invalidation, and per-engine PPGTT activation; an
explicit simulated backend; a GGTT-backed page-directory provider for the CPU
PPGTT model; and a staged independent-device ownership lifecycle with reverse
teardown, recovery, and poison-on-cleanup-failure are implemented and tested.
The integration audit corrected `PP_DCLV`, `PP_DIR_BASE`, `PP_DIR_BASE_READ`,
and `INSTPM` to per-engine offsets for render, video, and blitter, made
deactivation best-effort, pinned live directories, and added ownership-state
refresh before start. `make stage3a-test` and `make stage3a-sanitize-test`
pass. No real MMIO, PCI ownership, GGTT aperture ownership, physical
page-table publication, or hardware page walk is implemented. See
`docs/design/HASWELL_GEN75_STAGE3A_READINESS.md`.

The latest source audit added strict CLI numeric parsing and argument-bound
tests, fixed the NT-inspired Unicode shim's bounded-scan condition, and routed
compat ioctls through Linux's pointer-conversion helper. A device-scoped
page-table memory contract and separate i915-assisted/native DMA backend
contracts are now compile- and unit-tested; these tests use synthetic tokens
and do not call the Linux DMA API. On 2026-11-20, `make test`, shell/Python
syntax checks, the kernel-module build, and ASan/UBSan model and contract tests
passed. The later i915 resource-backend verification also passed `make test`,
the backend ASan/UBSan test, GCC `-fanalyzer`, and shell/Python syntax checks.
The real GEM resource smoke test exercised i915 GEM advice and activity without
submission. The later hardware execution evidence is recorded below and in
`SESSION_LOG.md`.

The subsequent project-wide audit passed `make test`, the active kernel Kbuild
target, GCC `-fanalyzer` and standalone syntax checks for active and legacy
user-space sources, shell/Python syntax checks, and ASan/UBSan for the PPGTT,
page-table contract, and GEM backend tests. The archived prototype's stale UAPI
include paths and aggregate build wiring were corrected; its complete build and
cleanup passed. No kernel module was loaded; `/dev/idw`-dependent checks skipped
because it was unavailable. See `SESSION_LOG.md` for the detailed test
boundary.

The integrated memory-lifecycle core is now connected to the public-ioctl
i915-assisted backend. On 2026-11-20, `make gpu-memory-manager-test` completed
three full allocation/bind/submit/wait/unbind/destroy lifecycles on Haswell
`8086:0412` with i915. The manager owns the lifecycle records and references;
i915 owns GEM backing, context VMs, actual PPGTT mappings, translation and
execution. Simulated ioctl tests cover timeout/quiescence and cleanup failures;
the physical run did not inject a timeout or invalidation failure. The QEMU
EDU guest test has now validated the native DMA provider against virtual
device-side transfers; physical Haswell DMA remains unvalidated. Hardware IDW
PPGTT and independent GPU ownership are not implemented.

## Eight-stage development sequence

| Stage | Milestone | Proof required | Status |
|---|---|---|---|
| 1 | GPU resource read/write | GPU writes data into a managed resource; CPU reads back the exact result. | **Passed, i915-assisted:** Haswell `8086:0412` wrote `0x2468ace0` at offset 16; CPU PREAD verified it after GEM_WAIT. i915 owns execution and mappings. |
| 2 | Native DMA | IDW allocates and maps real DMA memory for its exclusively owned PCI device. | **In progress:** QEMU EDU virtual-device DMA, page-table DMA copy, and remove cleanup passed under TCG. Physical Haswell DMA remains unproven and i915 remains bound. See `tests/qemu-dma/evidence-2026-11-20-tcg.log` and `tests/qemu-dma/evidence-2026-11-20-tcg-page-table-corrected.log`. |
| 3 | Hardware PPGTT | GPU uses IDW-created Haswell Gen7.5 page tables. | **Stage 3A software infrastructure implemented; physical path not implemented.** The CPU model has GGTT-resident directory modeling, separate 40-bit PDE/39-bit PTE encoders, scratch-PDE retention, and active/quiesced publication rules. A checked GGTT allocator, Haswell GTTADDR/flush/invalidate/per-engine activation abstraction with per-engine saved-register rollback and a simulated backend, a pinned GGTT directory provider, and an independent-device ownership/teardown/recovery lifecycle with ownership refresh are tested. Real PCI ownership, MMIO, platform GGTT aperture ownership, cache policy, and a physical page walk remain missing; i915 owns hardware mappings. See `docs/design/HASWELL_GEN75_STAGE3A_READINESS.md` and `docs/design/HASWELL_GEN75_PHYSICAL_PPGTT_READINESS.md`. |
| 4 | Independent submission | IDW submits and completes commands without i915 graphics execution. | Not implemented. |
| 5 | Scheduler | Multiple contexts, queues, timeout and recovery. | Not implemented independently. |
| 6 | UMD | Real shader compilation and rendering. | Not implemented. |
| 7 | Display/KMS | IDW-controlled display pipeline. | Not implemented. |
| 8 | Ownership transition | Safely detach i915 and give IDW exclusive hardware control. | Not attempted; i915 remains bound and owns the GPU. |

| Area | Current status | Reality |
|---|---|---|
| GPU discovery / PCI identity | working | Reads sysfs and DRM render-node information. |
| GEM object round-trip | working | Real i915 GEM create/write/read/close test; no GPU execution. |
| GEM allocation lifetime stress | working when run | Bounded multi-object create/write/read/verify/reverse-close test via i915; IDW does not own or schedule these objects. |
| Live i915 telemetry | working where exported | Reads available sysfs/debugfs counters; unavailable values remain unavailable. |
| Kernel control device / query ioctl | working scaffold | `/dev/idw` returns ABI, feature and state fields; `idwctl kernel` queries it. No PCI binding or register access. |
| IDW resource-handle lifecycle | working | Per-open IDR namespace, monotonically increasing non-reused handles, create/destroy ioctls, per-file quota and automatic cleanup on close. `resource-test` covers stale-handle rejection. |
| System-memory allocation backing | working CPU-memory milestone | Opt-in zeroed `vmalloc_user` backing with shared mmap; destroy returns busy while mapped and VMA references preserve backing through file close. It is not DMA-pinned, GPU-local, or mapped into a GPU VM. |
| V: IDW-native miniport lifecycle | working scaffold | Registration plus AddDevice/StartDevice/CreateAllocation/StopDevice/RemoveDevice callbacks; disposable test miniport exercises lifecycle. Allocation is metadata only, not GPU memory. |
| IDW backed allocation manager | partial | Owns CPU system-memory backing and retained imported dma-buf references. It does not allocate GEM objects or map backing into a GPU VM. |
| i915-assisted GEM resource backend | implemented in user space; real non-submission GEM smoke test passed | `src/i915_gem_resource_backend.cpp` creates/closes real GEM objects through i915 ioctls, tracks object handles and size, WILLNEED/DONTNEED retained/discardable/purged results, `GEM_BUSY` in-flight activity samples, and GPU VAs returned by successful execbuffer binding. The `gem-resource-test` passed real advice/recovery, activity-query, data-verification, and close operations without submitting a GPU command. `gpu-memory-test` also passed; no purge occurred in that run. This is not a kernel DRM backend, GPU residency query, DMA mapping provider, or IDW-owned PPGTT. |
| U: NT primitive shim | scaffold | Linux-backed allocation, spinlock and Unicode-string helpers; no Windows NT ABI, IRQL emulator, registry implementation or full pool semantics. |
| V: DXGKRNL-inspired dispatcher | scaffold | IDW-native callback dispatcher exists; it is not the Windows `DxgkInitialize` ABI and does not load a Windows miniport. |
| W: PCIe/BAR mediation | research | No BAR mapping, passthrough, or hardware ownership. |
| GPU command submission (i915 path) | implemented and hardware-tested; i915-assisted only | `idwctl gpu-submit` uses the injectable `GpuSubmissionOps` boundary, validates the Haswell two-dword `MI_BATCH_BUFFER_END` + `MI_NOOP` batch, submits it to the i915 render engine, validates the returned 4 KiB-aligned GPU VA, waits up to 5 seconds with GEM_WAIT, and closes the batch/context on success and failures. Final hardware run: 3/3 completions on Haswell `8086:0412`; returned GPU VA `0x011a66000`. Simulated tests cover malformed input, submission failure, timeout, partial creation, and cleanup errors. This is not IDW-owned scheduling or execution. |
| GPU VM / hardware PPGTT | delegated to i915; IDW CPU model exists | The hardware test uses an i915 context; real address-space creation and page-table management remain inside i915. IDW's separate CPU-only table model is not a GPU mapping and does not program Haswell hardware. |
| GPU memory management (first validation step) | i915-assisted resource/backing and fixed-VA submission tests passed; native VidMm/residency manager not implemented | `gem-resource-test` validates real GEM create/close, advice, activity and data round-trip without submission. `gpu-memory-test` was run on Haswell/i915: it bound resource and batch objects at `0x10000000` and `0x10100000`, completed no-op submissions before and after advice, destroyed/recreated the context, and rebound at the same VAs. No purge was observed in this run. `GEM_BUSY` reports in-flight activity, not residency. `retained` reports backing existence at the ioctl; DONTNEED makes backing discardable but does not force purge. i915 owns PPGTT, device DMA and residency/placement. |
| Backend-neutral allocation/VM lifecycle core | software implementation; simulated-backend tests pass | `MemoryManager` composes backend backing ownership, GPU-VA reserve/release, bind/unbind, invalidation retry, submission reference counting, wait/quiescence recovery, and destroy guards. `make memory-manager-test` tests complete success and failure paths. The callback core does not itself implement residency/eviction or hardware mappings. |
| i915-assisted integrated memory manager | implemented and hardware-tested; i915-owned | `I915AssistedMemoryBackend` connects managed backing, pin/bind, command submission, wait/quiesce, CPU dword access, context-destroy unbind, and GEM cleanup through public i915 ioctls. `make gpu-memory-manager-test` completed three resource lifecycles at `0x10000000`, `0x10001000`, and `0x10002000`. `make gpu-resource-write-test` passed on Haswell `8086:0412`: GPU VA `0x10000010`, initial value `0x13579bdf`, submitted value/readback `0x2468ace0`. Simulated tests cover malformed command encoding, bounds, failed exec/wait/readback, retained submission references, quiescence, and cleanup. i915 owns backing, mapping, PPGTT, translation, execution and scheduling; native DMA and IDW PPGTT remain unimplemented. |
| GPU-VA interval allocator | CPU-only software model implemented | `src/gpuva_model.c` supports bounded first-fit allocation, alignment, guards, fixed reservations, overlap rejection, and release. It does not own allocations/backing, residency, map references, or hardware GPU addresses. |
| Haswell Gen7.5 PPGTT CPU model | software table model corrected; physical/hardware validation not run | `src/haswell_ppgtt_model.c` builds 512 PDE slots with 1024-entry/4 KiB PTE tables, uses separate PDE (`0xff0`, `< 2^40`) and PTE (`0x7f0`, `< 2^39`) encoders with Haswell uncached policy, keeps a CPU shadow separate from a GGTT-resident directory provider, performs PDE writes/reads through GTTADDR callbacks, supports checked map/unmap and software translation, rejects PDE publication or reclamation while active, keeps empty-table PDEs valid, reclaims through a zeroed scratch table, defers old-table free until matching invalidation acknowledgement, coalesces dirty ranges by generation, and poisons submission checks after invalidation failure. Hand-derived golden vectors cover upper address bits, PTE cacheability bit 11, 39-bit/40-bit boundaries, misalignment, overflow, and reserved/32 KiB flags. Synthetic DMA addresses and CPU translation are software tests only. |
| Page-table memory backend boundary | contracts implemented; provider passed virtual DMA validation | `page_table_memory.h` tags page/mapping addresses with device-generation scope and lifetime cookies. The separate internal `idw_dma_memory` provider uses `dma_alloc_coherent` only for an exclusively IDW-driver-bound PCI device, enforces 40-bit DMA range/alignment, and tracks page cookies. `kernel/idw_edu_dma_test_main.c` bound to QEMU EDU (`1234:11e8`), completed 32 verified 4 KiB DMA read/write round trips, rejected invalid descriptors and mask/owner cases, and freed an outstanding provider page during PCI remove. The run used QEMU 11.1.3 TCG, not KVM; it validates the virtual DMA path only, not physical Haswell DMA. |
| DMA-backed page-table storage adapter | software implementation; virtual DMA evidence | `src/pt_dma_adapter.c` wraps an injectable DMA page provider, validates zeroed 4 KiB coherent/streaming pages, 40-bit alignment, device/generation scope, cookies, and outstanding-page cleanup, and exposes page-table-only memory/PPGTT ops. `IDW_PT_BACKEND_PAGE_TABLE_ONLY` is rejected by the complete native contract. `make pt-dma-adapter-test` covers validation, provider-failure rollback, synchronization, cleanup, and PPGTT table allocation/free with a simulated provider. The QEMU EDU probe copies a populated Gen7.5 page table through device DMA and verifies its layout; this passed on 2026-11-20 under QEMU 10.0.13 TCG with Debian kernel 6.12.117 (`tests/qemu-dma/evidence-2026-11-20-tcg-page-table-corrected.log`). This is storage and virtual-DMA evidence only: no resource mapping, invalidation, VM activation, MMIO, or hardware page walk. |
| GGTT allocator and residency | software implementation; simulated tests pass | `src/ggtt_allocator.c` provides checked first-fit interval allocation over a caller-supplied aperture, 4 KiB granularity, alignment, unique cookies, fixed reservations, pin/resident/bind state, stale-free rejection, and teardown refusal. `make ggtt-allocator-test` covers success, failure, exhaustion, and teardown paths. This does not own or program the physical GGTT aperture. |
| Haswell GTT hardware abstraction | software implementation with simulated backend; no MMIO | `include/idw/haswell_gtt_hw.h` / `src/haswell_gtt_hw.c` define GTTADDR read/write, GGTT flush, TLB invalidation, and per-engine PPGTT activation/deactivation with per-engine saved-register rollback and poison-on-uncertain-state. Register offsets and masks are explicit, including ring-relative `PP_DCLV`, `PP_DIR_BASE`, `PP_DIR_BASE_READ`, and `INSTPM` for render, video, and blitter. The test backend is a software simulator; it is not hardware evidence. |
| GGTT-backed PPGTT directory provider | software implementation; model integration tested | `src/ggtt_directory.c` allocates a pinned, resident 4 KiB GGTT directory, zeroes and writes PDEs through GTTADDR, flushes, unpins, and frees it. The test connects the corrected PPGTT model to the provider and verifies the PDE in simulated GTT, translation, activation, reclamation, pinning, and teardown. No physical GPU consumes the directory. |
| Independent-device ownership lifecycle | software implementation; simulated stages tested | `src/device_lifecycle.c` rejects `start()` while i915 is bound or ownership is not exclusive, supports refreshing those observations with `idw_device_lifecycle_set_ownership()` before start, optionally requires `8086:0412`, runs ordered stages, rolls back in reverse, recovers only after clean teardown, and poisons on cleanup failure. It does not unbind i915, map a BAR, or touch hardware. |
| Haswell hardware PPGTT | not implemented; CPU model corrected, physical path absent | No hardware page-table publication, GPU TLB invalidation, context activation, MMIO, independent page walk, or physical GPU test. The 2026-11-20 PRM audit found four CPU-model blockers: PDE/PTE encodings differ, mapped pages are 39-bit while page tables may be 40-bit, the page directory must be GGTT-resident and written through GTTADDR, and live PDE removal is unsafe because directory entries are cached. Those software findings are corrected and tested. A real GGTT allocator, MMIO GTTADDR path, register-level invalidation/activation, cache-policy selection, coherency/IOMMU work, and physical evidence remain missing; i915 remains the hardware owner. See `docs/design/HASWELL_GEN75_PHYSICAL_PPGTT_READINESS.md`. |
| X: IDW command/fence translation | not implemented | The i915-assisted manager tracks internal submission tickets through completion, but IDW does not yet translate DXGKRNL-style submissions or expose a hardware-independent fence timeline. |
| P/Q: DRM/KMS integration | research / existing Linux dependency | Uses i915 DRM UAPI for safe observation and GEM smoke test. |
| R/S/T: WDDM/UMD/WSL2 study | research | No Windows `.sys` loading or UMD execution. |
| Y/Z: diagnostics/benchmarks | partial | Basic telemetry only; structured crash/fuzz/benchmark suites remain. |

Never label a scaffold as implemented. Do not unload or unbind `i915` to test this control-plane module.


## DRM/GEM-backed resource references (new)

IDW now supports importing an exported dma-buf descriptor into an IDW per-file allocation handle. The kernel takes and retains a `dma_buf` reference, accounts its exported size against the per-file quota, and releases the reference on explicit handle destruction or file close. `idwctl dmabuf-test` exercises the i915 GEM -> PRIME dma-buf export -> IDW import -> close userspace dma-buf fd -> destroy IDW handle -> close GEM handle lifecycle.

This is genuine shared-object lifetime integration, not an IDW-owned GEM allocator: i915 remains the exporter, performs the backing allocation, and owns GPU mapping and execution. IDW does not map the dma-buf, attach it to a GPU VM, or claim device ownership. Stage 1 now separately verifies a GPU write/readback through the integrated manager; the eight-stage sequence above records what remains before independent control.


## GPU execution milestone (0.7.0)

`idwctl gpu-submit [count]` is an explicit, opt-in Haswell hardware test. It requires PCI `8086:0412` with i915 bound, creates an i915 GEM context and 4 KiB batch object, uploads the exact Haswell two-dword sequence `MI_BATCH_BUFFER_END` + `MI_NOOP` padding, submits with `DRM_IOCTL_I915_GEM_EXECBUFFER2`, validates the returned nonzero 4 KiB-aligned GPU VA, waits up to five seconds with `DRM_IOCTL_I915_GEM_WAIT`, and attempts object/context cleanup on both success and failure. The batch is intentionally a no-op. The callback contract is isolated from i915 ioctl code so a future backend can replace the resource/submit/wait operations without reusing Gen8+ command encoding.

Hardware evidence: the final sequential `IDW_GPU_SUBMIT_COUNT=3 make gpu-test` completed all three submissions on the current Haswell/i915 device; `GEM_WAIT` succeeded and the final reported GPU VA was `0x011a66000`. This is i915-assisted execution, not IDW-owned scheduling, mappings, or GPU ownership.

The i915 context is the GPU-VM boundary for this milestone. On supported Haswell configurations, i915 manages the context's PPGTT/address-space bindings. IDW does not program page tables, select raw GPU virtual addresses, map imported dma-bufs into a VM, or own fences. Therefore the three requested areas are now connected at the safe i915 API boundary, but native IDW VM management and DXGKRNL-to-i915 submission/fence translation remain future work.

Run explicitly with `make gpu-test` or `./build/idwctl gpu-submit 1`. Do not run as root unless your DRM permissions require it. Do not include this test in unattended generic smoke tests because it submits work to physical hardware.


## Resource-backed context VM integration (0.8.0)

`idwctl gpu-resource-test` ties the previously separate dma-buf lifetime and GPU submission tests together. An i915 GEM resource is exported as dma-buf, retained by an IDW allocation handle, and included alongside a no-op batch in an i915 `GEM_EXECBUFFER2` submission using a dedicated context. i915 consequently binds the resource into that context's GPU VM for the submission; the returned object offset is reported and the batch completion is waited. This is an i915-managed context binding, not an IDW-owned VM mapping and not a query of physical residency. IDW still does not allocate GPU virtual addresses, directly manipulate Haswell PPGTT page tables, issue a command that dereferences the resource, or translate Windows/DXGKRNL submissions.

## GPU memory-management validation (first step)

`idwctl gpu-memory-test` is an explicit Haswell/i915 hardware test. It creates context and GEM resource/batch objects through the i915-assisted resource backend, asks i915 to bind them at fixed 4 KiB-aligned context GPU virtual addresses using `EXEC_OBJECT_PINNED`, verifies/tracks the returned offsets, submits and waits for the harmless batch, destroys the context to release its VM bindings, and then exercises `I915_MADV_DONTNEED` / `I915_MADV_WILLNEED`. Each ioctl's `retained` output reports whether backing exists at that ioctl; recovery is triggered if either reports `retained == 0`, covering purge both before and between the calls. If backing was purged, the test closes and recreates the resource rather than trying to reuse lost contents. It then verifies the data and rebinds the resource and batch at the same GPU VAs in a new context. `GEM_BUSY` records an in-flight activity sample and is not a residency query. These are real i915-managed GPU addresses and mappings; i915 owns the PPGTT page tables, device DMA mappings and placement/residency. `DONTNEED` only makes backing discardable under memory pressure and is not a forced-eviction test. This validation backend passed on Haswell at `0x10000000` / `0x10100000`; purge was not observed. It is not an IDW VidMm or PPGTT implementation.

Run only when explicitly testing hardware: `make gpu-memory-test` or `./build/idwctl gpu-memory-test`. It is excluded from generic smoke tests because it submits work to physical hardware.


## 0.9.0 — Complete-stack contract baseline

Added compile-checked internal C interface contracts for the NT runtime, DXGK-inspired adapter/device dispatcher, VidMm allocation operations, GPU VM mapping operations, VidSch submission/fence operations, hardware backend capability/probe operations, and IDW UMD message protocol. Added `docs/design/STACK_CONTRACTS.md` with the ownership model, dependency graph, required invariants, explicit exclusions and evidence-based completion gates. These interfaces are **design contracts only**; they do not implement the referenced managers, Windows D3DKMT compatibility, shader compiler, native Haswell KMD, or an independent stack.

The contract headers compile with strict C11 warnings enabled using `make contract-test`. This test does not require libdrm or kernel headers. A duplicated mutex acquisition in the metadata allocation ioctl path was also removed; the old code would have self-deadlocked on allocation creation.

## 1.0.0 — System-backed allocation foundation

UAPI 0.4 adds opt-in `IDW_ALLOCATION_CREATE_SYSTEM_BACKED` allocations. IDW allocates zeroed, page-aligned kernel system memory and exposes a shared mapping keyed by the allocation handle. Explicit destroy returns busy while mapped; file-close cleanup drops the handle reference while VMA references preserve backing until unmap. Metadata-only and imported dma-buf handles cannot use this mapping. Resource tests passed on the target kernel, including stale-ID rejection and access after fd close. This is CPU system memory only: there is no DMA pinning, GPU VM mapping, residency, or GPU ownership.


## Historical sequence note

The eight-stage sequence near the beginning of this document supersedes the
earlier seven-item roadmap. Stage 1 is complete only for i915-assisted resource
read/write. It does not complete independent GPU memory management, native DMA,
hardware PPGTT, or GPU ownership.


## 1.1.0 — DMA-backed page-table storage adapter

`idw_pt_dma_adapter` connects an injectable DMA page provider to the Haswell
Gen7.5 software page-table model. It allocates and validates zeroed 4 KiB pages,
checks 40-bit DMA range and alignment, enforces device/generation scope and
allocation cookies, zeroes pages before use, synchronizes page tables for the
device, and requires explicit release of every page before teardown. It
advertises the explicit `IDW_PT_BACKEND_PAGE_TABLE_ONLY` profile, which the
complete native contract rejects.

The kernel binding `kernel/idw_dma_pt_adapter.c` supplies those pages from the
existing ownership-checked `idw_dma_memory` provider using `dma_alloc_coherent`
and DMA synchronization. The QEMU EDU test copies a populated 4 KiB page
table to a second DMA page and verifies the exact bytes and Gen7.5 PTE layout
after device-side DMA. This passed on 2026-11-20 under QEMU 10.0.13 TCG with
Debian kernel 6.12.117
(`tests/qemu-dma/evidence-2026-11-20-tcg-page-table-corrected.log`).
The check is page-table storage visibility through QEMU's virtual PCI DMA
model, not Haswell PPGTT. There is still no resource mapping,
GPU TLB invalidation, VM activation, MMIO, hardware page walk, or independent
GPU ownership.

## 1.3.0 — Corrected Haswell Gen7.5 PPGTT model

Corrected the CPU-side model using the physical-readiness audit. PDE and PTE
encoding are now separate: PDEs carry page-table address bits 39:32 at 11:4,
while PTEs carry page address bits 38:32 at 10:4 and cacheability bit 3 at bit
11. Page tables are limited to `< 2^40`; mapped pages are limited to `< 2^39`.
Reserved PDE bits and the 32 KiB page-size bit are rejected explicitly.

Added a GGTT-resident directory provider contract with allocation, GTTADDR
PDE write/read, flush, and release callbacks. The model keeps a separate CPU
shadow; `idw_hsw_ppgtt_pde()` reads the shadow and
`idw_hsw_ppgtt_hardware_pde()` reads through GTTADDR. Unmap no longer removes a
PDE. Empty tables remain reachable until a quiesced
`idw_hsw_ppgtt_reclaim_empty()` points the PDE at a zeroed scratch table; the
old table is freed only after the next invalidation completes. PDE publication
and reclamation are rejected while the model is active, and invalidation
failure still poisons the VM.

Added hand-derived golden-vector tests for PDE/PTE upper address bits, PTE
cacheability bit 11, 39-bit and 40-bit boundaries, misalignment, overflow,
reserved/32KB flags, GGTT directory residency, GTTADDR writes, active-PDE
rejection, deferred reclamation, and rollback. The QEMU EDU page-table storage
test now uses the corrected PTE encoder and independent expected values; it
passed under QEMU 10.0.13 TCG with Debian kernel 6.12.117. Evidence is at
`tests/qemu-dma/evidence-2026-11-20-tcg-page-table-corrected.log`.

No hardware state was changed. i915 remains bound and is the physical GPU
owner. Stage 3 remains not implemented: there is no real GGTT allocator, MMIO
GTTADDR path, register-level invalidation, VM activation, or physical page-walk
evidence.

## 1.2.0 — Physical Haswell PPGTT readiness audit

Added `docs/design/HASWELL_GEN75_PHYSICAL_PPGTT_READINESS.md`, an
evidence-separated audit against the Intel Haswell PRM (Memory Views and
Command Reference: Registers) and Linux v6.12 i915. It documents the physical
page-directory/page-table formats, alignment and address-width rules, cache
coherency, invalidation, register ownership, safe i915-bound validation, and a
minimal reversible page-walk experiment for a future exclusively owned device.

The audit confirmed the current two-level geometry but identified four
CPU-model blockers:

1. PDE and PTE encodings differ. PDE bits 11:4 carry physical address 39:32;
   PTE bits 10:4 carry physical address 38:32 and PTE bit 11 is cacheability
   control bit 3. The pre-correction shared `0xff0` encoder was not valid for
   PTEs.
2. Mapped data pages are limited to 39-bit addresses; page tables may be
   addressed below 2^40. The pre-correction model and adapter accepted 40-bit
   mapped pages.
3. The page directory must be entirely within the GGTT and PDEs must be
   modified through GTTADDR. The pre-correction model kept `directory[512]` in
   CPU memory and had no GGTT provider.
4. Live PDE removal is not hardware-safe because the directory cache cannot be
   invalidated on the fly. The pre-correction model cleared and retired PDEs;
   i915 keeps the PDE valid and clears PTEs to a scratch entry instead.

These software findings were corrected in 1.3.0. The physical blockers remain.

Register-level activation and invalidation remain absent and i915-owned:
`PP_DCLV`, `PP_DIR_BASE`, per-engine PPGTT enable, `INSTPM` TLB invalidate plus
sync flush, and `GFX_FLSH_CNTL_GEN6`. No hardware state was changed, i915 was
not unbound, and no physical page walk is claimed. Stage 3 remains not
implemented; the next physical gate is exclusive ownership, a real GGTT
allocator, and MMIO register work, not a physical run.
