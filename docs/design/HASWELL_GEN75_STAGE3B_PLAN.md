# Haswell Gen7.5 Stage 3B — native hardware integration plan

Status: design only. No physical hardware operation is authorized or claimed.
The target GPU (`8086:0412`, `0000:00:02.0`) remains bound to Linux i915.

## 1. Objective

Prepare the smallest reversible path from Stage 3A software infrastructure to a
native Haswell driver that can own the GPU long enough to prove:

1. IDW owns the PCI device and its BARs.
2. IDW allocates a real GGTT range.
3. IDW publishes a Gen7.5 page directory through GTTADDR.
4. IDW invalidates the relevant TLBs.
5. The GPU performs one controlled page walk using IDW-created tables.

Stage 3B is not a claim that any of those steps is complete. It defines the
prerequisites, order, and abort conditions.

## 2. Current boundary

Stage 3A provides tested software for:

- GGTT interval allocation, residency, pinning, binding, and teardown refusal.
- Pinned GGTT-resident page directories with GTTADDR PDE write/read and flush.
- Per-engine `PP_DCLV`, `PP_DIR_BASE`, `PP_DIR_BASE_READ`, `INSTPM`, and
  `RING_MODE_GEN7` handling for render, video, and blitter.
- Per-engine save/restore, best-effort deactivation, poison-on-uncertain-state,
  and checked teardown that refuses active or invalidation-pending models
  unless hardware quiescence is explicitly asserted after reset.
- An ownership lifecycle that rejects start while i915 is bound or ownership is
  not exclusive, with refreshable ownership observations.

Stage 3A does not provide PCI ownership, BAR mapping, forcewake, power wells,
real MMIO, real GGTT aperture ownership, cache-policy programming, IOMMU
integration, hardware reset, or physical page-walk evidence.

## 3. Work packages

| WP | Deliverable | Required prerequisites | Safe software work now | Physical gate |
|---|---|---|---|---|
| 3B.1 | PCI probe/remove lifecycle and ownership claim | A tested i915 detach/rebind procedure, exclusive PCI ownership plan, IOMMU/VT-d policy, firmware and runtime-PM ownership, recovery path on probe failure | PCI ownership state machine, fake PCI resource model, probe/remove fault injection | First actual PCI claim |
| 3B.2 | BAR discovery and mapping | 3B.1 plus validated BAR indices/sizes, no i915 mapping conflict, MMIO cache policy, mapping lifetime rules | BAR descriptor parser, mapping contract, fake MMIO backend, mapping fault tests | First BAR map/unmap |
| 3B.3 | GGTT aperture discovery | 3B.2 plus platform aperture base/size, stolen-memory/DSM handling, GTTMMADR location, no overlap with i915-owned ranges | Aperture descriptor parser, overlap validator, synthetic aperture tests | First real aperture read |
| 3B.4 | Real GTTADDR access | 3B.2 and 3B.3 plus correct GTT aperture mapping, write-combining/cache attributes, posting-read and flush rules | GTTADDR backend contract, fake GTT memory, write/readback and flush fault tests | First GTTADDR PDE write/read |
| 3B.5 | Device-scoped DMA allocation and synchronization | 3B.1 plus DMA API ownership, 40-bit mask, IOMMU domain, page-table lifetime, mapping cookies | Extend QEMU/virtual provider coverage and adapter failure tests | First real DMA allocation and device access |
| 3B.6 | PPGTT activation and invalidation | 3B.2 through 3B.5 plus forcewake, power wells, engine idle/quiesce, cache policy, per-engine register access | Cache-policy abstraction, engine-idle state machine, activation/invalidation fault tests | First per-engine PPGTT enable |
| 3B.7 | Reset, timeout, and recovery | 3B.1 plus GPU reset path, PCI reset, forcewake release, watchdog/timeout policy, post-reset reinitialization | Reset/timeout state machine, poison/recovery tests, bounded-wait validator | First controlled reset |
| 3B.8 | Reversible first page-walk experiment | All prior gates plus exclusive ownership, known-good recovery image, serial console, register dump tooling, and a disposable test system | Experiment harness, preflight validator, abort-path tests with mocks | First physical page walk |

## 4. Dependency order

```text
3B.1 PCI ownership
  -> 3B.2 BAR mapping
       -> 3B.3 GGTT aperture
            -> 3B.4 GTTADDR
                 -> 3B.6 PPGTT activation
3B.1 -> 3B.5 DMA --------------------^
3B.1 -> 3B.7 reset/recovery ----------+
All -> 3B.8 first page-walk experiment
```

No physical step may skip its prerequisites. A failure at any gate must return
the device to i915 ownership or leave it in a documented, non-driving state.

## 5. Safe software tasks that can start now

These do not touch the physical GPU and should be implemented before any
hardware claim:

1. Haswell PPGTT cache-policy abstraction for `GAC_ECO_BITS` and `GAM_ECOCHK`,
   with read-modify-write, save/restore, and simulated failure tests.
2. A PCI ownership/probe/remove state machine with fake resources and
   rollback tests, including the i915-bound rejection path.
3. A BAR descriptor and mapping contract with a fake MMIO backend, bounds
   checks, and unmap-order tests.
4. A GGTT aperture descriptor parser and overlap validator using synthetic
   platform data.
5. A bounded MMIO wait/poll helper for forcewake, flush, and invalidation
   completion, with timeout and poison tests.
6. An engine-idle/quiesce state machine that gates PPGTT activation and
   deactivation.
7. Integration tests that connect the ownership lifecycle, directory provider,
   hardware abstraction, and checked teardown in one failure-injection matrix.

## 6. First hardware experiment preconditions

The experiment must not start unless all are true:

- A disposable Haswell test system is available and recoverable.
- The exact i915 detach/rebind procedure has been rehearsed and documented.
- Exclusive PCI ownership is confirmed, not assumed.
- The BAR and GGTT aperture descriptors match the platform.
- A known-good recovery image and serial console are available.
- The experiment has a hard timeout and a register-dump capture path.
- The test uses a scratch GGTT range and a scratch page table that no i915
  context references.
- The abort path disables PPGTT, restores saved registers, flushes, releases
  forcewake, and returns the device to i915.

## 7. Explicit blockers

- i915 currently owns the GPU and must not be unbound by this project without a
  separately approved ownership-transition procedure.
- No real BAR, GTT aperture, forcewake, power-well, cache-policy, IOMMU, or
  reset path is implemented.
- Stage 2 physical native DMA remains unproven.
- There is no physical page-walk evidence and no independent GPU execution.
- A failed teardown must never be reported as success; the current software
  policy is to poison and retain conservative active state.

## 8. Exit criteria for Stage 3B

Stage 3B is complete only when the physical experiment produces preserved
evidence of:

- Exclusive IDW PCI ownership and successful recovery to i915.
- A real GGTT range allocated and released.
- A PDE written through GTTADDR and read back correctly.
- Per-engine invalidation completion.
- One controlled GPU page walk using IDW-created Gen7.5 tables.
- Clean teardown with no leaked GGTT range, page table, DMA page, or forcewake
  reference.

Until then, Stage 3 remains not implemented and all hardware claims remain
unsupported.
