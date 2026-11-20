# Haswell Gen7.5 Stage 3A implementation readiness

Status: software infrastructure implemented and tested. No physical hardware
operation was performed. i915 remains bound and owns the physical GPU.

This document covers the Stage 3A gate only: the software pieces needed before
an independent Haswell PCI driver may attempt GGTT allocation, GTTADDR
publication, invalidation, or per-engine PPGTT activation. It is not evidence
of a hardware page walk.

## 1. Scope

Implemented in this milestone:

1. A checked GGTT interval allocator with residency, pin, binding, and teardown
   state.
2. A hardware abstraction for GTTADDR reads/writes, GGTT flush, TLB
   invalidation, and per-engine PPGTT activation/deactivation.
3. A simulated hardware backend for deterministic software tests.
4. A GGTT-backed page-directory provider that connects the corrected CPU PPGTT
   model to the allocator and hardware abstraction.
5. A staged independent-device ownership lifecycle with reverse-order
   teardown, recovery, and poison-on-cleanup-failure.
6. Focused tests, including activation rollback and stale-free rejection.

Not implemented and not claimed:

- PCI binding, unbinding i915, BAR mapping, forcewake, power wells, GPU reset,
  interrupts, or engine/ring setup.
- Real MMIO or GTTADDR access.
- Real GGTT aperture ownership or a platform-derived Haswell aperture size.
- Cache-policy programming (`ECOCHK`, `GAC_ECO_BITS`), IOMMU integration, or
  physical page-table publication.
- A physical Haswell page walk, independent execution, or ownership transfer.

## 2. Evidence levels

| Level | What it means | This milestone |
|---|---|---|
| CPU-only software model | Data structures and callbacks; no device access. | Yes. |
| QEMU virtual PCI DMA | A virtual device performs DMA. | Not rerun here; prior page-table DMA evidence remains separate. |
| Simulated Haswell MMIO | Software state machine emulates register/GTT effects. | Yes, explicitly labeled simulated. |
| i915-assisted physical GPU | i915 owns mappings and execution. | Not changed. |
| Independent physical Haswell | IDW owns the device and the GPU consumes IDW tables. | Not attempted. |

The simulated backend in `src/haswell_gtt_hw.c` is not hardware evidence. Its
statistics count callback invocations, not register transactions.

## 3. GGTT allocator and residency

Files:

- `include/idw/ggtt_allocator.h`
- `src/ggtt_allocator.c`

The allocator is platform-neutral. The caller supplies the aperture base,
size, page size, and allocation-record capacity. The allocator does not assume
a Haswell aperture size.

Properties:

- 4 KiB page granularity and power-of-two alignment.
- First-fit allocation over sorted interval records.
- Nonzero, unique cookies for live allocations; free requires the exact
  offset, byte count, and cookie, so a stale free with a different cookie
  cannot release a reused range.
- Fixed-offset reservations for pre-existing aperture regions.
- Explicit `resident` state, pin count, and optional DMA binding with
  device/generation scope.
- Pinned allocations are resident and cannot be freed or unbound.
- Bound allocations cannot be freed until unbound.
- `idw_ggtt_destroy()` refuses to destroy a live allocator.
- Overflow, misalignment, duplicate-cookie, overlap, and out-of-range
  requests are rejected.

The allocator does not decide eviction policy and does not issue hardware
operations. A future VidMm/residency manager owns policy above this layer.

## 4. Hardware abstraction

Files:

- `include/idw/haswell_gtt_hw.h`
- `src/haswell_gtt_hw.c`

The low-level operation boundary is:

```c
read32 / write32
gtt_read32 / gtt_write32
gtt_flush
invalidate_tlb(engine_mask)
set_engine_ppgtt(engine, enable)
posting_read
release
```

The real backend must own PCI BAR mapping, forcewake, register serialization,
and the actual INSTPM poll. The abstraction computes the Haswell per-engine
register offsets (`RING_INSTPM`, `RING_PP_DIR_DCLV`, `RING_PP_DIR_BASE`,
`RING_PP_DIR_BASE_READ`, and `RING_MODE_GEN7`) from the render, blitter, and
video ring bases; the backend maps those offsets to the owned BAR.

High-level operations:

- `idw_hsw_hw_write_pde()` / `idw_hsw_hw_read_pde()` validate a 4 KiB-aligned
  directory, 512-entry bounds, and the GGTT range, then use GTTADDR.
- `idw_hsw_hw_flush_gtt()` calls the GGTT flush callback and a posting read at
  `GFX_FLSH_CNTL_GEN6`.
- `idw_hsw_hw_invalidate()` validates the engine mask, calls the TLB-invalidate
  callback, and performs a posting read at each selected engine's `INSTPM`
  register.
- `idw_hsw_hw_activate()` flushes the GGTT, snapshots each selected engine's
  `PP_DCLV` and `PP_DIR_BASE_READ`, writes `PP_DCLV = 0xffffffff` and the
  directory GGTT offset to each selected engine's `PP_DIR_BASE`, enables PPGTT
  on each requested engine, and invalidates the engine TLBs. It does not write
  only the render-engine aliases at `0x2220`/`0x2228`.
- `idw_hsw_hw_deactivate()` disables the active engines, invalidates, restores
  each engine's saved `PP_DCLV`/`PP_DIR_BASE`, flushes, and posts. Every step is
  attempted even after an earlier failure; any failure poisons the abstraction
  and keeps the active state conservative.
- Activation failure rolls back enabled engines and restores the saved
  per-engine registers. A rollback failure poisons the abstraction. An
  invalidation failure during activation always poisons because TLB state is
  uncertain. A failed GTT read, GTT write, or GTT flush also poisons because
  hardware visibility is uncertain.
- Once poisoned, all subsequent operations return `-EIO`.

Register offsets and masks are defined in the header, not hardcoded in callers.
The abstraction does not program cache-policy registers; that remains an
explicit prerequisite for a real backend.

## 5. GGTT-backed directory provider

Files:

- `include/idw/ggtt_directory.h`
- `src/ggtt_directory.c`

The provider implements `struct idw_hsw_ppgtt_directory_ops`:

- `alloc_directory`: allocates one 4 KiB, page-directory-tagged, resident and
  pinned GGTT range, zeroes all 512 PDE slots through GTTADDR, and flushes. The
  pin prevents the allocator from releasing a hardware-visible directory while
  the model owns it. Cookie collisions with another provider sharing the same
  allocator are retried with the next cookie.
- `free_directory`: refuses and poisons the provider while the hardware
  abstraction reports active PPGTT, so a live directory is not zeroed or
  released. After quiescence it zeroes the slots through GTTADDR, flushes,
  unpins, and frees the GGTT range. Teardown continues to the free step even if
  zeroing/flushing fails, and marks the provider poisoned.
- `write_pde_gttaddr` / `read_pde_gttaddr`: validate the directory scope and
  use the hardware abstraction.
- `flush_gtt`: flushes through the hardware abstraction.
- Initialization rejects a provider scope that does not match the hardware
  abstraction's device/generation scope with `-EXDEV`.

This connects the corrected CPU PPGTT model to a GGTT-visible directory rather
than a CPU array. It still does not make the physical GPU consume that
directory.

## 6. Independent-device ownership lifecycle

Files:

- `include/idw/device_lifecycle.h`
- `src/device_lifecycle.c`

The lifecycle is a staged state machine:

```text
DORMANT -> INITIALIZING -> READY -> QUIESCING -> STOPPED
                  |                       |
                  +------ FAILED <--------+
```

Ownership gates:

- `i915_bound == true` rejects `start()` with `-EBUSY`.
- `exclusive == false` rejects `start()` with `-EPERM`.
- `idw_device_lifecycle_set_ownership()` refreshes the observed i915-bound and
  exclusive-ownership state before `start()`; it is rejected once the lifecycle
  is ready or initializing. The caller must refresh observations after init and
  immediately before claiming the device.
- The optional Haswell GT2 identity check requires `8086:0412`.
- The caller supplies the device/generation scope.

Stage execution:

- Stages run in order.
- A failed stage rolls back completed stages in reverse order.
- A successful rollback leaves the lifecycle `FAILED` but recoverable.
- A failed rollback poisons the lifecycle.
- `recover()` tears down remaining completed stages and returns to `DORMANT`
  only when cleanup succeeds.
- A teardown failure poisons the lifecycle; recovery cannot report success.
- `destroy()` rejects live, completing, or poisoned lifecycles.

This module never unbinds i915 or touches PCI. The caller must supply truthful
ownership observations and must not set `exclusive` without actual ownership.

## 7. Tests

Focused targets:

```sh
make stage3a-test
make stage3a-sanitize-test
```

Targets:

- `make ggtt-allocator-test`
- `make haswell-gtt-hw-test`
- `make device-lifecycle-test`

Coverage:

- GGTT first-fit, alignment, fixed reservations, overlap, duplicate cookies,
  stale frees, pin/resident/bind, teardown refusal, and exhaustion.
- Haswell register golden offsets/masks for all three engine bases, GTTADDR
  PDE read/write, directory residency and pinning, per-engine
  activation/deactivation with save/restore, engine masks, per-engine INSTPM
  barriers, failed-engine rollback without poison, GTT-write and invalidation
  failure poison, and invalid ranges.
- PPGTT model integration through the GGTT directory provider, including a
  mapped PDE visible in simulated GTT, translation, activation, reclaim,
  device-scope mismatch rejection, and teardown.
- Ownership rejection with i915 bound, non-exclusive ownership, and wrong PCI
  identity; refreshed ownership observations before start; successful staged
  start; reverse teardown; init-failure rollback; cleanup-failure poison;
  recovery; and destroy guards.
- Checked PPGTT model teardown refuses active or invalidation-pending models
  unless the caller asserts hardware quiescence after reset.
- Directory release refuses while hardware is active; duplicate provider
  cookies are retried; deactivation failure keeps active state conservative,
  restores registers best-effort, poisons, and blocks destroy until reset.

Executed results on 2026-11-20:

```text
PASS: GGTT allocator, residency, binding, and teardown tests
PASS: Haswell GGTT directory, hardware sequence, and rollback tests
PASS: independent-device ownership, teardown, and recovery tests
```

ASan/UBSan with leak detection passed for all three focused tests. The kernel
module build passed with `make kernel CC=gcc W=1`; no module was loaded.

The full `make test` run reached all software targets successfully, then stopped
at `tests/smoke.sh` because this environment has no Intel DRM render node. That
is an environment limitation, not a Stage 3A test failure. No physical GPU test
was run.

## 8. Remaining Stage 3 gates

Before a real backend may program hardware:

1. Implement PCI probe/claim, BAR mapping, forcewake, power-well, reset, and
   interrupt ownership, with save/restore and recovery.
2. Obtain the real GGTT aperture base/size from the owned device and prove
   exclusive GGTT ownership.
3. Implement a real MMIO/GTTADDR backend for the per-engine offsets defined by
   the abstraction.
4. Resolve Haswell cache policy (`ECOCHK`, `GAC_ECO_BITS`), `PP_DCLV`
   semantics, coherency/IOMMU behavior, and the `PGTBL_CTL2` open question.
5. Add locking, sleepability, and callback-context rules for the real backend.
6. Prove page-table publication, invalidation completion, and per-engine
   activation on a disposable system with saved register evidence.
7. Only then run the minimal physical page-walk experiment and preserve the
   serial log, register dump, PDE/PTE readback, and CPU data readback.

Until those gates are met, Stage 3 remains not implemented. The software model
and simulated backend are preparation, not hardware proof. The Stage 3B
prerequisite and dependency plan is in
`docs/design/HASWELL_GEN75_STAGE3B_PLAN.md`.
