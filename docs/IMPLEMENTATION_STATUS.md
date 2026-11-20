# Implementation status

Status labels: **working** = exercised by the current project; **scaffold** = source/design exists but not a complete subsystem; **research** = requires hardware/ABI research; **not implemented** = no functional implementation.

**Hardware validation state:** On 2026-11-20, both explicit i915-assisted
execution tests passed on Haswell `8086:0412` (`/dev/dri/renderD128`, driver
i915). `gpu-test` completed three no-op render submissions; each returned a
4 KiB-aligned GPU VA and successful `GEM_WAIT`. `gpu-memory-test` bound at
`0x10000000` / `0x10100000`, completed the initial and rebound submissions,
and released the context. No purge was observed during that run. This confirms
the i915-assisted execution path only; no independent IDW GPU control is
claimed. Passing software checks is evidence about software behavior, not
independent GPU control.

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
| GPU-VA interval allocator | CPU-only software model implemented | `src/gpuva_model.c` supports bounded first-fit allocation, alignment, guards, fixed reservations, overlap rejection, and release. It does not own allocations/backing, residency, map references, or hardware GPU addresses. |
| Haswell Gen7.5 PPGTT CPU model | software table model implemented; physical/hardware validation not run | `src/haswell_ppgtt_model.c` builds 512 PDE slots with 1024-entry/4 KiB PTE tables, uses the upstream Gen6/7 address transform and valid bits with Haswell uncached policy, supports checked map/unmap and software translation, rolls back on allocation/duplicate table-DMA failures, defers empty-table reclamation until matching invalidation acknowledgement, coalesces dirty ranges by generation, and poisons submission checks after invalidation failure. Synthetic DMA addresses and CPU translation are software tests only. |
| Page-table memory backend boundary | device-scope contract and validators implemented; no real DMA provider | `page_table_memory.h` tags page/mapping addresses with device-generation scope and lifetime cookies. The i915-assisted profile reports GEM/VM-bind/exec-wait capabilities only and rejects DMA-page-table requests. The native DMA contract requires exclusive device ownership and complete allocation/map/sync/invalidation/activation operations; its validator is structural only. Synthetic contract tests do not allocate DMA memory. |
| Haswell hardware PPGTT | not implemented | No device DMA mapping, DMA synchronization, hardware page-table publication, GPU TLB invalidation, context activation, MMIO, independent page walk, or physical GPU test. i915 remains the owner. |
| X: IDW command/fence translation | not implemented | The i915 client test exercises submission and completion, but IDW does not yet translate a DXGKRNL-style submission or expose IDW fences. |
| P/Q: DRM/KMS integration | research / existing Linux dependency | Uses i915 DRM UAPI for safe observation and GEM smoke test. |
| R/S/T: WDDM/UMD/WSL2 study | research | No Windows `.sys` loading or UMD execution. |
| Y/Z: diagnostics/benchmarks | partial | Basic telemetry only; structured crash/fuzz/benchmark suites remain. |

Never label a scaffold as implemented. Do not unload or unbind `i915` to test this control-plane module.


## DRM/GEM-backed resource references (new)

IDW now supports importing an exported dma-buf descriptor into an IDW per-file allocation handle. The kernel takes and retains a `dma_buf` reference, accounts its exported size against the per-file quota, and releases the reference on explicit handle destruction or file close. `idwctl dmabuf-test` exercises the i915 GEM -> PRIME dma-buf export -> IDW import -> close userspace dma-buf fd -> destroy IDW handle -> close GEM handle lifecycle.

This is genuine shared-object lifetime integration, not an IDW-owned GEM allocator: i915 remains the exporter, performs the backing allocation, and owns GPU mapping and execution. IDW does not map the dma-buf, attach it to a GPU VM, submit commands, or claim device ownership. The next work follows the seven-step sequence in the README: GPU memory management, Gen7.5 PPGTT, engine submission, scheduler, UMD, display, then ownership transition. The first opt-in i915 validation test is described below; it does not claim IDW-owned GPU-VA management.


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


## Development sequence

1. GPU memory management: real GPU virtual addresses, page-table mappings, residency and eviction.
2. PPGTT: implement the correct Haswell Gen7.5 mechanisms, not Gen8+ assumptions.
3. Hardware submission: engine-specific ring buffers, command execution and completion fences.
4. Scheduler: contexts, queues, preemption limitations, timeouts and recovery.
5. UMD: resource management, shader compilation and Gen7.5 command generation.
6. Display: independent integration with the display hardware and KMS.
7. Ownership: safely transition away from i915 only after the independent backend is ready.

The current first-step validation is i915-delegated. Do not mark GPU memory management complete until IDW owns the allocation/backing lifecycle, actual GPU mappings, residency and eviction policy with tests that prove those behaviors.
