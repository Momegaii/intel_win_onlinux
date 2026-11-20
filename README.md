# IDW — Intel Graphics / Windows-Kernel Architecture Project

**Target:** Intel HD Graphics 4600 (Haswell GT2, PCI `8086:0412`, revision `06`) on Arch Linux. Current hardware owner: Linux `i915`.

IDW is a Windows-inspired graphics-kernel architecture project for Linux. The A–Z reference in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) is the project's backbone and is intentionally preserved. This repository is an engineering workbench, not a claim that a complete WDDM implementation already exists.

## Current capabilities

- Discovers Intel DRM render nodes and reports PCI identity / active kernel driver.
- Performs a real i915 GEM object create, write, read-back, verify and close test.
- Provides an integrated allocation/VA/bind/submit/completion/unbind/destroy
  lifecycle through an i915-assisted backend; the explicit Haswell test passes
  with i915 retaining hardware ownership.
- Displays live i915 telemetry where the running kernel exports it.
- Provides a desktop properties GUI and an optional read-only debugfs snapshot helper.
- Provides an optional `/dev/idw` resource/control plane and IDW-native miniport lifecycle dispatcher. It supports metadata-only handles, opt-in kernel system-memory-backed allocations with shared userspace mappings, and retained dma-buf references. A disposable test miniport exercises AddDevice → StartDevice → CreateAllocation(metadata only) → StopDevice → RemoveDevice. It does not register a PCI driver, map BARs, program the GPU, or compete with i915.
- Preserves the earlier synthetic prototype under `legacy-prototype/` for historical reference only.

**Important:** System-backed allocations are CPU-visible system memory only; they are not pinned for DMA, mapped into a GPU VM, or GPU-local memory. GEM round-trip is real kernel-managed memory-object activity, not GPU command execution. IDW currently does not load Windows `.sys` files, implement a complete `ntoskrnl` ABI, implement `dxgkrnl`, independently submit GPU work, or own hardware fences. The integrated lifecycle manager tracks submissions through its i915-assisted backend, but i915 remains the hardware submission and completion owner. See [`docs/IMPLEMENTATION_STATUS.md`](docs/IMPLEMENTATION_STATUS.md).

## Build on Arch Linux

```bash
sudo pacman -S --needed base-devel gcc libdrm pkgconf python tk
make
make test
make install-user
```

Run:

```bash
~/.local/bin/idwctl info
~/.local/bin/idwctl selftest
~/.local/bin/idwctl kernel  # optional; requires /dev/idw
~/.local/bin/idw-gpu-properties
```

Optional telemetry helper:

```bash
sudo make install-system-helper
```

Optional kernel control-plane module (requires matching kernel headers):

```bash
make kernel
sudo insmod kernel/idw_core.ko
sudo ./build/idwctl kernel
sudo rmmod idw_core
```

The module creates `/dev/idw` with mode `0600`. It is intentionally not loaded automatically. It does not bind to `00:02.0` and does not access GPU registers. `idwctl resource-test` checks metadata handles, stale-ID rejection, mmap rejection for metadata-only handles, and the lifetime of opt-in CPU system-memory mappings. The test miniport can be loaded after `idw_core.ko` to exercise the callback lifecycle; its allocation callback creates metadata only, not a GEM object or GPU allocation. Build/load it only if matching kernel headers are installed. Do not run the module build with a different kernel's headers.

## Hardware identity

The supplied system report identifies:

- PCI function: `0000:00:02.0`
- Vendor/device: `8086:0412`
- Subsystem: `1043:8534`
- Kernel driver: `i915`
- Render node: `/dev/dri/renderD128` (node numbering can change)
- GPU: Haswell GT2 / Intel HD Graphics 4600

Haswell is Gen 7.5. Do not assume Gen8+ address-space or command-stream features. Keep `i915` as owner until an IDW backend has a complete, validated initialization, memory, submission, interrupt, reset and recovery path. Never allow IDW and i915 to own the same device simultaneously.

## Project layout

- `src/` — user-space discovery and i915 GEM smoke-test client.
- `gui/` — desktop properties application.
- `tools/` — read-only telemetry helper.
- `include/uapi/` — versioned IDW ioctl contract.
- `kernel/` — safe query-only kernel control-plane skeleton; no PCI binding.
- `compat/` — initial NT primitive wrapper design; not a Windows ABI.
- `legacy-prototype/` — old synthetic prototype, not for hardware use.
- `docs/ARCHITECTURE.md` — preserved lettered project backbone.
- `docs/IMPLEMENTATION_STATUS.md` — explicit implementation truth table.
- `tests/` — smoke tests.

## Development rules

1. No fake completion: simulated fences must be named simulated.
2. No fake memory: counters are not GPU virtual mappings or physical allocations.
3. No Windows binary loading: Windows KMDs depend on Windows kernel ABI/services.
4. No premature i915 unbind: establish full device ownership and recovery before considering a native backend.
5. Every new ioctl must be versioned, validated, bounded, and tested for 32/64-bit compatibility.
6. Keep Haswell-specific code separated from later Intel generations.
7. Treat kernel crashes as blockers; use a test VM/QEMU before experiments that can reset or wedge the physical GPU.

## Correct development sequence

1. **GPU memory management:** real GPU virtual addresses, page-table mappings, residency and eviction.
2. **PPGTT:** implement the correct Haswell Gen7.5 mechanisms, not Gen8+ assumptions.
3. **Hardware submission:** engine-specific ring buffers, command execution and completion fences.
4. **Scheduler:** contexts, queues, preemption limitations, timeouts and recovery.
5. **UMD:** resource management, shader compilation and Gen7.5 command generation.
6. **Display:** independent integration with the display hardware and KMS.
7. **Ownership:** safely transition away from i915 only after the independent backend is ready.

The i915-assisted `gpu-memory-test` verifies fixed GPU-VA placement in a real
Haswell context and exercises i915 GEM backing advice. It does **not** implement
IDW's VidMm, hardware PPGTT, forced eviction, or native GPU ownership; i915
remains the sole hardware owner. Run it explicitly with `make gpu-memory-test`.

The independence-first implementation phases, Gen7.5 PPGTT architecture,
hardware-backend interfaces, failure/rollback rules, and validation gates are
specified in [`docs/design/HASWELL_GEN75_PPGTT_PLAN.md`](docs/design/HASWELL_GEN75_PPGTT_PLAN.md).
The CPU-only GPU-VA interval allocator and Haswell Gen7.5 PPGTT software model
are implemented in `src/gpuva_model.c` and `src/haswell_ppgtt_model.c`; run
`make ppgtt-model-test` for their unit tests. They use synthetic DMA addresses
in tests and do not allocate device DMA memory, program hardware page tables,
invalidate GPU translations, or enable hardware access. See the page-memory
contracts below for the backend boundary.

The page-table memory boundary now has device-scoped DMA/page descriptors and
two explicit backend contracts. The i915-assisted profile reports GEM backing,
dma-buf lifetime, context VM binding, and exec/wait only; it rejects native
DMA-backed PPGTT capabilities. The future native backend requires exclusive
device ownership and complete DMA, synchronization, invalidation, and
activation operations. `make backend-contract-test` tests these boundaries
with synthetic descriptors; it does not allocate DMA memory or validate GPU
visibility. i915 remains the sole physical GPU owner.

The first integrated memory-lifecycle core is in
`include/idw/memory_manager.hpp` and `src/memory_manager.cpp`. It composes
backend-owned backing lifetime with the CPU GPU-VA allocator, binds/unbinds
through callbacks, retains VA reservations until invalidation succeeds, holds
resource references across submissions, and requires backend quiescence before
releasing references after an uncertain wait. `make memory-manager-test`
exercises the full lifecycle, including rollback, timeout, invalidation retry,
and destruction guards with a simulated backend. The separate i915-assisted
adapter and its hardware test are described below; neither backend makes this
an IDW-owned PPGTT or DMA implementation.

`make i915-memory-backend-test` runs simulated public-ioctl callbacks against
the same integrated manager/backend path. The opt-in
`make gpu-memory-manager-test` runs the path on Haswell with i915: the manager
creates three tracked GEM backings, reserves distinct VAs, pins each object in
its own i915 context by submitting a harmless batch, then submits another
harmless batch with that resource included in the execbuffer and waits for
`GEM_WAIT`. It then destroys each context (releasing its i915-managed VM
bindings) before destroying the allocation and closing its GEM handle. The
batch does not dereference resource contents. The observed hardware run
completed all three submissions at `0x10000000`, `0x10001000`, and
`0x10002000`. This is real i915-assisted lifecycle integration, not
IDW-controlled PPGTT or DMA.

Failure injection is software-only: callback tests cover failed waits followed
by quiescence, context-destroy retry, invalidation retry in the manager model,
unexpected VA recovery, and backing cleanup. i915 context destruction is the
backend's unbind/invalidation boundary; the current public path has no separate
IDW invalidation ioctl. The physical run did not induce timeout or invalidation
failure.

The user-space i915 GEM resource backend in
`src/i915_gem_resource_backend.cpp` creates/closes GEM objects, tracks
`WILLNEED`/`DONTNEED` backing-advice results and `GEM_BUSY` activity samples,
and records context GPU offsets returned by successful i915 execbuffers. Run
`make i915-resource-test` for simulated-ioctl state/error tests or
`idwctl gem-resource-test` for a real GEM create/advice/activity/data/close
smoke test without GPU command submission. This backend does not expose
i915's DMA mappings, PPGTT tables, or physical residency. The real GEM
resource smoke test passed on the current i915 device.

The execution milestone adds an injectable submission boundary in
`include/idw/gpu_submission.hpp` with an i915 implementation in `idwctl`.
`make gpu-submission-test` covers malformed Haswell batches, submission and
wait failures/timeouts, GPU-VA validation, and cleanup. The batch is exactly
two dwords: `MI_BATCH_BUFFER_END` plus `MI_NOOP` padding, for the i915-required
8-byte batch length. `make gpu-test` is an explicit hardware test; it creates
an i915 context and GEM batch, submits to the Haswell render engine, verifies
the returned 4 KiB-aligned GPU VA, waits for `GEM_WAIT` completion, and closes
the GEM handle/context. `make gpu-memory-test` separately validates fixed-VA
context bindings and advice/rebind behavior.

Both hardware tests passed on Haswell `8086:0412` with i915 owning the GPU:
three render no-op batches completed, and the fixed-VA test bound at
`0x10000000` / `0x10100000` and completed the initial and rebound submissions.
This proves the i915-assisted path executed and completed these harmless
batches; it does not demonstrate IDW-owned scheduling, mappings, PPGTT,
residency, DMA, or independent GPU execution. `GEM_BUSY` remains an activity
sample, not a residency query.

## IDW 1.0.0 — system-backed allocation milestone

The internal C contracts under `include/idw/contracts/` now define the boundaries for NT runtime, DXGK-inspired dispatcher, VidMm, GPU VM, VidSch, Haswell hardware backend and IDW UMD protocol. `docs/design/STACK_CONTRACTS.md` records the full layer map, ownership rules, dependency graph and acceptance gates. These are compile-checked interfaces, **not completed implementations**. Run `make contract-test` to validate the headers without libdrm or kernel headers.

The UAPI is version 0.4. `IDW_ALLOCATION_CREATE_SYSTEM_BACKED` opts into zeroed, page-aligned kernel system memory mapped through a shared `/dev/idw` mapping. Destroy returns `EBUSY` while mapped; closing the file drops its handle reference, and VMA references keep the backing alive until unmap. Default allocation creation remains metadata-only; dma-buf imports remain exporter-backed references. This is a VidMm foundation, not GPU memory residency, DMA mapping, or GPU VM support.

The public ioctl layout has an automated 64-bit native ABI test covering structure sizes and field offsets. The runtime resource test passed on the target kernel, including stale-handle rejection, metadata-only mmap rejection, shared-memory access after handle/fd close, and dma-buf lifetime. This is not proof of 32-bit compatibility; that gate will require both architectures.

## Next engineering gates

The CPU-only GPU-VA allocator and Gen7.5 PPGTT model are foundations, not
completion of Phase 1 or 2. Continue in this order:

1. Complete the GPU memory manager: backing/object lifetime, map references,
   budgets, pinning, residency, eviction, and concurrency/failure tests.
2. Complete the Haswell Gen7.5 PPGTT path: DMA-backed page-table memory,
   publication/synchronization, invalidation, and activation, with exact PRM
   checks before any hardware use.
3. Add engine-specific hardware submission and completion fences.
4. Add VidSch contexts, queues, timeouts, and recovery.
5. Build the independent UMD, resource management, and shader/compiler path.
6. Integrate display/KMS independently.
7. Complete the required compatibility work, then consider ownership transition
   only when the independent backend is ready and tested on an isolated device.

The detailed phase boundaries, including GGTT/DMA and interrupt/fence gates,
are maintained in
[`docs/design/HASWELL_GEN75_PPGTT_PLAN.md`](docs/design/HASWELL_GEN75_PPGTT_PLAN.md).
ABI, lifetime, cleanup, rollback, and reproducible-test safeguards remain
required throughout; do not treat a software-model pass as hardware evidence.

The target is a genuine architecture, not a wrapper wearing a Windows name. The route there is incremental, measured, and honest about what runs on silicon.


### GEM allocation lifetime test

`idwctl memory-test [count] [size-bytes]` creates a bounded set of real i915 GEM objects, writes and reads a verification pattern, and closes every handle (including cleanup after a partial failure). Defaults are 8 objects of 65536 bytes. This exercises DRM object lifetime through the existing i915 driver; it is not an IDW-owned allocator, GPU virtual-memory manager, or command-submission path.


### IDW resource-handle lifecycle (kernel module)

When `idw_core.ko` is loaded, each open `/dev/idw` file receives a private resource namespace. `IDW_IOCTL_ALLOCATION_CREATE` creates a quota-limited IDW metadata handle; `IDW_IOCTL_ALLOCATION_DESTROY` releases it. Handle IDs are monotonically increasing and not reused during the file's lifetime, so a stale ID cannot identify a later allocation. Closing the file reclaims all remaining handles. `idwctl resource-test [count] [size-bytes]` exercises this lifecycle and stale-ID rejection. These are **IDW-owned resource descriptors only**: they reserve accounting metadata, not system RAM, VRAM, GGTT/PPGTT space, or i915 GEM objects.


### DRM/GEM dma-buf import

With the optional kernel module loaded, run `idwctl dmabuf-test`. It creates an i915 GEM object, exports it through DRM PRIME as a dma-buf, imports that descriptor into an IDW allocation handle, closes the userspace dma-buf descriptor, then destroys the IDW handle and closes the GEM handle. This validates retained dma-buf lifetime. i915 remains the exporter and GPU-memory owner; this is not IDW-owned GPU virtual memory or command submission.


## GPU execution test (IDW 0.7.0)

Build with `make`, then explicitly run `./build/idwctl gpu-submit 1` (or `make gpu-test`). This creates an i915 context, submits a harmless Gen7 `MI_BATCH_BUFFER_END` batch on the render engine, waits for completion, and destroys its GEM object/context. It is a real hardware submission through i915. The test does not render pixels or modify display state.

The i915 context provides the kernel-managed GPU address-space boundary; i915 owns Haswell PPGTT setup, mappings, scheduling and completion fences. IDW does not directly write PPGTT page tables or yet translate Windows/DXGKRNL submissions into i915 requests. The dma-buf import layer retains object references but does not yet map imported objects into that context.


## IDW 0.8.0 — resource-backed context VM binding

`idwctl gpu-resource-test` is an explicit, opt-in hardware integration test. It creates an i915 GEM resource, exports it as a PRIME dma-buf, imports that dma-buf into an IDW allocation handle (which retains a kernel `dma_buf` reference), closes the userspace dma-buf fd, creates an i915 context, and submits a harmless batch with both the batch BO and resource BO in the execbuffer object list. i915 binds the submitted objects into that context's GPU VM as part of execbuffer processing; the test reports the returned resource offset and waits for the batch BO completion before releasing the IDW reference and GEM handles.

This closes a real lifetime-to-context-binding integration loop, but the ownership boundary remains explicit: IDW retains the dma-buf reference; i915 performs the GPU-VM mapping, PPGTT page-table updates, scheduling and fence signaling. The no-op batch does not dereference or modify the resource. The returned offset is an i915-assigned VM offset for this submission, not an IDW-managed GPU virtual address or a residency query.

Run only when `/dev/idw` is loaded and accessible: `make gpu-resource-test` or `./build/idwctl gpu-resource-test`. This hardware test is intentionally excluded from `make test`.
