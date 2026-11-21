# IDW complete-stack contracts (design baseline 1.0)

This document defines the subsystem boundaries that every implementation must satisfy. It is a contract and dependency map, **not evidence that the listed subsystems are already implemented**. The existing A–Z reference in `../ARCHITECTURE.md` remains unchanged and authoritative for the project's topic organization.

## Product target

An IDW-owned graphics stack for Intel HD Graphics 4600 / Haswell GT2 (`8086:0412`) on Arch Linux. The long-term target is an independent stack: IDW UMD + IDW kernel graphics manager + Haswell backend. Linux i915/Mesa may be used as a temporary validation backend, but they must not be misrepresented as IDW-owned VidMm/VidSch/PPGTT or UMD functionality.

## Layer map

```text
Applications / API frontends (future: Vulkan, Direct3D compatibility)
                 |
       IDW UMD + shader compiler (S)
                 |
       IDW UMD protocol (versioned)
                 |
  DXGKRNL-inspired dispatcher (V)
       |         |          |
     VidMm     VidSch     NT runtime (U)
       |         |          |
    Memory      GPU VM   execution/DPC model
       \         |          /
        Haswell hardware backend (K/L/M/N/O)
                 |
        PCI / BAR / DMA / IRQ
                 |
       Intel HD Graphics 4600
```

### Temporary Linux validation backend (not the final architecture)

```text
IDW user client -> IDW kernel resource/control contracts
               -> i915 DRM UAPI -> i915 -> Haswell GPU
```

This backend can validate object lifetime and real submission while i915 owns the hardware. It cannot be counted as IDW's own scheduler, memory manager, page-table manager, command streamer, or independent KMD.

## Subsystem contracts and ownership

| Subsystem | Owns | Must not own / must not assume | Contract header |
|---|---|---|---|
| NT runtime | NT-style pool/string/status primitives and documented execution-context rules | Windows kernel ABI equivalence; name-only IRQL emulation | `nt_runtime.h` |
| DXGK dispatcher | adapter/device lifecycle, callback registration, version negotiation, callback lifetime | loading Windows KMD binaries or treating Linux callbacks as Windows DDI ABI | `dxgk_dispatch.h` |
| VidMm | allocation objects, budgets, residency, pin/evict policy, backing references | treating dma-buf refcount as residency or physical VRAM | `vidmem.h` |
| GPU VM | per-context GPU VA ranges, map/unmap, permissions, page-size/capability validation | confusing CPU page tables, VT-d IOMMU and GPU PPGTT | `gpuvm.h` |
| Page-table memory | device-scoped DMA addresses, page/mapping lifetime cookies, synchronization and backend capability validation | treating CPU physical addresses or another device's DMA addresses as GPU-visible | `page_table_memory.h` |
| VidSch | contexts, engine queues, dependencies, preemption capability, fence timeline and timeout/reset policy | reporting submission accepted as completed | `scheduler.h` |
| Haswell backend | PCI identity/capabilities, generation-specific memory and command rules, engine/interrupt/reset operations | using Gen8+ assumptions or concurrent i915 ownership | `hw_backend.h` |
| IDW UMD | API-facing objects, state translation, shader/compiler interface, command-buffer construction | direct privileged MMIO or bypassing kernel validation | `umd_protocol.h` |

## Required invariants

1. Every externally supplied structure begins with a size/version contract; reserved fields must be zero and unknown flags rejected.
2. Every handle is scoped to an owning file/device/session and cannot be reused after destruction without generation protection or equivalent stale-handle defense.
3. Resource lifetime is reference-counted across UMD, kernel allocation, VM binding, submissions and fences.
4. A resource cannot be destroyed while mapped or referenced by an in-flight submission. Destruction either waits, returns busy, or schedules deferred release with an explicit contract.
5. GPU VA mapping validates alignment, overflow, range, permissions and backend page-size support. Unmap must invalidate translations as required by the hardware/backend.
6. Submission validates engine, context, VM, batch bounds, alignment, command permissions and referenced resources before queueing.
7. Fence values are monotonic per timeline. A fence is signaled only after hardware/backend completion; error/reset completion is distinguishable from success.
8. Backend capability discovery is authoritative. Unsupported Haswell features must be reported unsupported, never emulated silently as hardware features.
9. Exactly one kernel driver owns PCI configuration, BARs, MMIO, DMA setup, interrupts and reset for the physical GPU at a time.
10. The i915 validation backend and independent Haswell backend are mutually exclusive ownership modes.
11. Windows D3DKMT/DDI binary compatibility is a separate compatibility project. IDW-native contracts are not binary-compatible by declaration.
12. No feature is marked `working` until build, automated tests and the relevant hardware/VM test pass and their logs are recorded.

## Dependency graph and implementation order

```text
ABI/UAPI + diagnostics
  -> NT runtime + object/handle manager
  -> adapter/device lifecycle dispatcher
  -> stage 1: GPU resource read/write (i915-assisted evidence passed)
  -> stage 2: native DMA allocation + mapping
  -> stage 3: IDW-created Haswell Gen7.5 hardware PPGTT
  -> stage 4: independent submission + completion
  -> stage 5: scheduler, queues, timeout + recovery
  -> stage 6: UMD, shader compilation + rendering
  -> stage 7: IDW-controlled display/KMS pipeline
  -> stage 8: safe exclusive ownership transition
```

The eight-stage proof gates are summarized in the README and
`docs/IMPLEMENTATION_STATUS.md`. Compatibility work in the A–Z architecture
continues alongside these hardware milestones. The detailed implementation
plan and Gen7.5 PPGTT evidence gates are in
`HASWELL_GEN75_PPGTT_PLAN.md`. The 2026-11-20 physical-readiness audit,
register ownership map, and reversible page-walk experiment are in
`HASWELL_GEN75_PHYSICAL_PPGTT_READINESS.md`; its CPU-side encoding,
GGTT-directory, and reclamation findings are now implemented in the software
model, but the physical plan is still not hardware validation. Independence is
the design target from stage 1; i915 is only a temporary validation owner.
Operational ownership restrictions may be relaxed only on an isolated test
system with exclusive device ownership. Do not trade away source validation,
lifetime rules, cleanup, rollback, or reproducible tests.

## Public/internal interfaces added in this milestone

`include/idw/contracts/` contains versioned C contracts for NT runtime, memory
manager, generic GPU VM, Haswell Gen7.5 DMA/page-table backend, scheduler,
DXGK-inspired dispatcher, hardware backend and UMD protocol. The
`i915_assisted.h` and `native_dma.h` contracts define mutually exclusive
backends: i915 exposes only its supported DRM/GEM validation path, while native
DMA requires exclusive ownership and device-scoped mappings. The generic
capability/scope validators and i915 profile are implemented and covered by
`make backend-contract-test`; native contract validation is structural only.
Page-table storage operations are implemented over a kernel DMA provider, but
resource mapping, invalidation, activation, and hardware PPGTT remain
unimplemented. Headers are compile-checked by `make contract-test`.

The separate CPU-only GPU-VA interval allocator and Haswell Gen7.5 PPGTT model
are implemented in `src/gpuva_model.c` and `src/haswell_ppgtt_model.c`, with
tests in `make ppgtt-model-test`. The model now has separate PDE/PTE encoders,
distinct 40-bit page-table and 39-bit mapped-page limits, a GGTT-resident
directory provider with GTTADDR PDE writes, and scratch-table retention with
deferred reclamation. A storage-only adapter
(`include/idw/pt_dma_adapter.h`, `src/pt_dma_adapter.c`) can supply pages from
an injectable DMA provider and is bound to the kernel DMA provider by
`kernel/idw_dma_pt_adapter.c`. Userspace tests still assign synthetic DMA
addresses; the QEMU EDU probe passed a corrected page-table storage copy check
through virtual DMA on 2026-11-20. None of these map resources for a device,
publish hardware page tables, invalidate GPU translations, or activate a VM.

Stage 3A adds the software prerequisites above that boundary:
`src/ggtt_allocator.c` provides checked GGTT interval allocation with
residency/pin/binding state; `src/haswell_gtt_hw.c` defines GTTADDR, flush,
invalidation, and per-engine activation/deactivation sequences with
saved-register rollback and poison-on-uncertain-state; `src/ggtt_directory.c`
connects the PPGTT model to a GGTT-backed directory; and
`src/device_lifecycle.c` provides staged exclusive-ownership, reverse teardown,
and recovery rules. The tests use a simulated backend. No PCI ownership, BAR
mapping, real MMIO, physical GGTT allocation, or hardware page walk is claimed.
See `HASWELL_GEN75_STAGE3A_READINESS.md`.

`include/idw/memory_manager.hpp` and `src/memory_manager.cpp` add a CPU-side
allocation lifecycle coordinator over a replaceable backend callback table.
It tracks backing objects, address reservations, backend bindings, invalidation
recovery, and submission references. A timed-out or failed wait does not drop
the reference; the backend must positively confirm quiescence before recovery
releases it. A separate public-ioctl i915-assisted adapter is connected and
hardware-tested; i915 continues to own the mappings and execution.
`HostMemoryBackend` now supplies a real IDW-owned host-RAM backing path through
the same callback table: a zero-faulted `memfd` mapping with CPU access and
synchronous host writes. It is not GEM, not DMA-mapped, not GPU-visible, and
does not populate or publish page tables. The fake test backend remains only a
deterministic test double. None of these paths is IDW-owned hardware memory
management or native DMA.

Page allocations and page-sized mapped addresses are tagged with an opaque
device scope and lifetime cookie. The CPU model rejects cross-scope table pages
and PTE mappings. These test tokens are synthetic; their validation does not
establish device identity, DMA mapping, synchronization, or GPU visibility.

## Completion gates

| Gate | Evidence required to pass |
|---|---|
| ABI baseline | 32/64-bit layout checks, version/size rejection tests, fuzzed ioctl inputs |
| Object model | handle stale-use tests, refcount tests, concurrent close/destroy tests, leak checks |
| NT runtime | pool/tag tests, string bounds tests, execution-context assertions, lockdep/KASAN |
| Dispatcher | registration rollback, callback ordering, unload/race tests, adapter/device lifecycle tests |
| VidMm | allocation backing, budget accounting, pin/evict/residency tests, in-flight reference tests |
| GPU VM | real map/unmap, guard/range tests, permissions, invalidation and address reuse tests |
| VidSch | multiple contexts, dependencies, timeout, reset/error fences, engine selection and fairness tests |
| UMD | protocol compatibility, shader compilation, command validation, resource binding, workload tests |
| Haswell KMD | PCI probe, BAR/DMA/IRQ lifecycle, Gen7.5 page tables, engine submission, reset/recovery, power management |
| Independent stack | end-to-end workload with i915 unbound and IDW sole owner, repeatable results and recovery tests |

## Explicit exclusions

- Loading `igdkmd64.sys`, `igd10iumd64.dll` or `igvk64.dll` directly into Linux.
- Claiming that WDK headers or Windows-driver-samples provide Microsoft's `dxgkrnl.sys` implementation.
- Claiming Mesa is equivalent to `dxgkrnl.sys` or that dxgkrnl replaces a vendor UMD.
- Direct Haswell PPGTT/MMIO writes while i915 owns the GPU.
- Calling an i915 context/execbuffer path an IDW-owned VidSch or VidMm implementation.
