# Haswell Gen7.5 Stage 3B physical readiness matrix

Status: design-closure audit. No physical GPU operation was performed. i915
remains bound to `0000:00:02.0` and owns the device.

This matrix separates what is implemented and unit-tested from what is only
simulated, validated under QEMU, validated through i915, or independently
validated on physical Haswell hardware. A higher evidence level is never
inferred from a lower one.

## 1. Evidence levels

| Code | Meaning |
|---|---|
| **IUT** | Implemented in this repository and covered by unit tests. |
| **SIM** | Exercised only through the software Haswell register/GTT simulator. |
| **QEMU** | Exercised against a QEMU virtual PCI device. QEMU does not model Haswell PPGTT. |
| **I915** | Exercised on physical Haswell through i915-owned GEM, context, PPGTT, DMA, and execution. This is not IDW ownership. |
| **PHYS** | Independently validated on physical Haswell with IDW owning the device, mappings, invalidation, and execution. |

## 2. Readiness matrix

| Prerequisite | Current implementation | Evidence | Status | Remaining blocker |
|---|---|---|---|---|
| PCI identity and ownership state machine | `idw_device_lifecycle_*` rejects start while i915 is bound or ownership is not exclusive; refreshes observations; reverse teardown; poison on rollback failure; missing-fini rollback now fails closed | IUT, SIM | Partial | No PCI probe/claim/remove, no i915 detach/rebind rehearsal, no exclusive ownership proof |
| PCI BAR discovery and safe mapping | Design only | None | Not started | BAR indices/sizes, MMIO cache attributes, mapping lifetime, conflict and recovery rules |
| GGTT aperture discovery and reserved regions | Platform-neutral interval allocator with pin/resident/bind, stale-free rejection, and teardown refusal | IUT, SIM | Partial | No platform aperture discovery, stolen/DSM handling, or reserved-range map |
| GTTADDR access and serialization | Hardware abstraction with GTTADDR read/write, bounds checks, posting reads, and simulator | IUT, SIM | Partial | No real MMIO mapping, forcewake, locking, or callback-context rules |
| GGTT flush | `idw_hsw_hw_flush_gtt()` calls the backend flush and posts `GFX_FLSH_CNTL_GEN6` | IUT, SIM | Partial | Backend must set `GFX_FLSH_CNTL_GEN6` bit 0, barrier, and wait; no real run |
| PPGTT cache policy | Registers defined but not programmed | None | Not started | `GAC_ECO_BITS` and `GAM_ECOCHK` policy, save/restore, and scope of override unresolved |
| Device-scoped DMA page-table allocation | DMA adapter requires exact 4 KiB, coherent/streaming, 40-bit, scoped pages; QEMU EDU provider passed virtual DMA | IUT, QEMU | Partial | No Haswell DMA allocation, IOMMU domain, or physical coherency proof |
| Page-table DMA synchronization | Model now requires `sync_page_table_for_device` and calls it after zeroing, PTE publication, and PTE clearing; sync-failure test poisons | IUT, SIM, QEMU storage | Partial | No physical non-snooped Haswell walk; IOMMU/coherency behavior unresolved |
| Page-directory publication | Pinned GGTT directory, GTTADDR zero/write/readback, flush, live-allocation revalidation, active-write rejection, and model integration | IUT, SIM | Partial | No physical GTT aperture or hardware-consumed directory |
| Per-engine PPGTT activation | Per-engine `PP_DCLV`, `PP_DIR_BASE`, `PP_DIR_BASE_READ`, `INSTPM`, `RING_MODE_GEN7`; exact activation trace; all-three-engine and partial-activation tests; `RING_MODE` snapshot/restore; pre-enabled PPGTT rejected; enable/disable readback verification | IUT, SIM | Partial | No real MMIO, forcewake, engine idle, cache policy, or load-completion stall |
| Per-engine invalidation | `INSTPM` TLB-invalidate/sync-flush contract and per-engine posting reads | IUT, SIM | Partial | Backend must poll `SYNC_FLUSH` with a bounded timeout and barriers; no physical run |
| Rollback and poison behavior | Activation rollback restores `PP_DCLV`, `PP_DIR_BASE`, and `RING_MODE`; failed rollback marks the device conservatively active and blocks destroy; deactivation failure keeps active state | IUT, SIM | Partial | No real engine/GPU reset path to clear a poisoned state |
| Engine quiescence and reset | No engine/ring ownership or reset implementation | None | Not started | Ring stop/idle, engine reset, GPU reset, forcewake release, post-reset reinit |
| i915 restoration | Ownership lifecycle only; no PCI transition | IUT, SIM | Not started for hardware | Rehearsed detach/rebind, known-good recovery image, serial console, watchdog |
| QEMU EDU virtual DMA baseline | Linux provider performed 32 verified 4 KiB DMA round trips and cleanup under QEMU TCG | QEMU | Passed | QEMU does not model Haswell PPGTT or IOMMU |
| i915-assisted physical baseline | Existing GEM, context, execbuffer, wait, and Stage 1/2 tests on Haswell | I915 | Passed previously | i915 owns mapping, PPGTT, DMA, and execution; not IDW evidence |
| Controlled GPU page walk | No physical publication or walk | None | Not started | All physical gates above |
| Independent GPU execution | None | None | Not started | Page walk, submission, scheduler, fences |

## 3. Register and ordering audit

| Item | Authoritative reference | Current result | Evidence |
|---|---|---|---|
| Render/video/blitter ring bases | i915 `intel_engine_regs.h`: `0x2000`, `0x12000`, `0x22000` | Match | Documentation/source + unit vectors |
| `PP_DCLV`, `PP_DIR_BASE`, `PP_DIR_BASE_READ`, `INSTPM`, `RING_MODE_GEN7` | Ring-relative `+0x220`, `+0x228`, `+0x518`, `+0xc0`, `+0x29c` | Match; per-engine use | Documentation/source + unit vectors |
| `PGTBL_CTL`, `GAM_ECOCHK`, `GAC_ECO_BITS`, `GFX_FLSH_CNTL_GEN6` | i915 `0x2020`, `0x4090`, `0x14090`, `0x101008` | Match | Documentation/source + unit vectors |
| Masks | `PP_DIR_DCLV_2G=0xffffffff`, PPGTT enable bit 9, TLB invalidate bit 9, sync flush bit 5, GGTT flush bit 0, cache-policy fields | Match | Documentation/source + unit vectors |
| Activation order | i915 writes `PP_DCLV`, `PP_DIR_BASE`, then `RING_MODE_GEN7`; invalidates before work | Current order: flush, snapshot, program, enable, verify, invalidate, post | SIM exact trace |
| Deactivation order | Disable, invalidate, restore saved registers, flush, post | Current order matches, with readback verification | SIM exact trace |
| PDE write visibility | i915 writes PDEs, `mb()`, reads back the last PDE, GGTT invalidate, `mb()` | Model writes each PDE and reads it back; provider flushes after the batch | IUT, SIM |
| Page-table visibility | Non-snooped GPU; explicit DMA sync required | Model syncs after zeroing, PTE publication, and PTE clearing | IUT, SIM |
| `PP_DIR_BASE` save/restore | i915 reads `PP_DIR_BASE` and `PP_DIR_BASE_READ` for diagnostics; `PP_DIR_BASE` is the programmed value | Fixed to snapshot `PP_DIR_BASE`, not `PP_DIR_BASE_READ`; test creates divergent readback | IUT, SIM |
| `RING_MODE_GEN7` save/restore | i915 sets only bit 9; other bits belong to the engine mode | Fixed to snapshot and restore the full register and reject pre-enabled PPGTT | IUT, SIM |
| Rollback failure state | A failed rollback must not be reported as inactive | Fixed: failed activation rollback marks the device conservatively active and blocks destroy | IUT, SIM |
| `PP_DIR_BASE` load completion | i915 command stream stores `PP_DIR_BASE` to scratch GTT to stall before `INSTPM` invalidate | Not implemented; no authoritative MMIO-only equivalent established | Unresolved |
| Cache-policy programming | i915 `gen7_ppgtt_enable()` sets `GAC_ECO_BITS` and `GAM_ECOCHK` | Not implemented | Blocker |
| `INSTPM` completion | i915 polls `SYNC_FLUSH` with a bounded wait | Delegated to the real backend; simulator does not model the poll | Contract only |
| `GFX_FLSH_CNTL_GEN6` | i915 writes bit 0 and posts a read | Delegated to the real backend; abstraction posts a read | Contract only |

## 4. Audit findings by severity

### Critical

1. **No exclusive physical ownership.** i915 still owns the device. No PCI
   claim, BAR mapping, forcewake, power-well, engine, or reset path exists.
2. **No cache-policy programming.** The real PPGTT walk policy is not selected
   or saved/restored. Activating PPGTT without this is not approved.
3. **No load-completion ordering for `PP_DIR_BASE`.** i915 uses a command-stream
   store to scratch GTT to stall before `INSTPM`. An MMIO-only equivalent has
   not been established from authoritative sources.

### High

4. **No real GTTADDR or GGTT aperture ownership.** The allocator and directory
   provider are platform-neutral software; no platform aperture or reserved
   ranges are discovered.
5. **No physical DMA/IOMMU proof.** QEMU EDU proves the Linux provider against a
   virtual device only. Haswell DMA and IOMMU behavior are unproven.
6. **No engine quiescence or reset.** A poisoned software state cannot be
   cleared on hardware; recovery depends on an unimplemented reset path.
7. **No i915 detach/rebind rehearsal.** The recovery path is a plan, not
   evidence.

### Medium

8. **PTE publication rollback is conservative, not transactional.** A sync
   failure after PTE writes poisons the model; it does not restore the previous
   PTEs. The device must be reset or torn down.
9. **`PP_DIR_BASE_READ` semantics remain unresolved.** The programmed
   `PP_DIR_BASE` is now used for save/restore, but the exact readback timing and
   meaning are not proven on hardware.
10. **`PGTBL_CTL2` and the maximum PPGTT VA profile remain unresolved.**
11. **Directory alignment is 4 KiB minimum; i915 uses 64 KiB.** The hardware
    requirement versus i915 policy is unresolved.

### Low

12. **Simulator aliases `PP_DIR_BASE` writes into `PP_DIR_BASE_READ`.** Tests
    now create divergence to avoid relying on the alias, but the simulator is
    still not a hardware model.
13. **The simulated `INSTPM` callback returns success without polling.** The
    real backend contract requires the poll; only the contract is tested.

## 5. GO / NO-GO

**Decision: NO-GO for independent Haswell hardware initialization.**

The software and design package is reviewable, but the physical entry criteria
are not met. The first hardware step must not be attempted until all of the
following are true:

1. A disposable Haswell system and a rehearsed i915 detach/rebind recovery path
   exist.
2. Exclusive PCI ownership is observed, not assumed.
3. BAR and GGTT aperture descriptors match the platform and are validated
   against reserved regions.
4. Real GTTADDR access, serialization, forcewake, and GGTT flush are implemented
   and tested on the disposable system.
5. Haswell DMA allocation, IOMMU policy, and non-snooped page-table coherency
   are proven.
6. PPGTT cache policy and `PP_DIR_BASE` load-completion ordering are resolved
   from authoritative sources and implemented.
7. Engine idle, timeout, reset, and i915 restoration paths are implemented and
   rehearsed.
8. A scratch GGTT range and scratch page table are isolated from every i915
   context, and the abort path restores saved registers and returns the device
   to i915.

Until then, Stage 3 remains **NOT IMPLEMENTED** and no physical page walk,
independent DMA, or independent GPU control is claimed.
