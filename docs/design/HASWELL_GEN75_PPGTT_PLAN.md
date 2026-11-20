# Haswell Gen7.5 PPGTT implementation plan

Status: **CPU-only GPU-VA allocator and Gen7.5 PPGTT software model implemented;
hardware backend remains unimplemented**. A backend-neutral page-table-memory
contract binds DMA addresses and page allocations to a device scope; the CPU
model rejects mismatched scopes. The i915-assisted profile advertises only the
current GEM/context validation surface and is explicitly rejected for
DMA-backed PPGTT. A structural contract for a future exclusive-owner DMA
backend is compile- and unit-tested, but has no hardware implementation. No
real DMA mapping, device synchronization, hardware page-table publication, GPU
invalidation, or GPU-side translation has been performed.

## Independence-first target

IDW's final GPU memory and page-table path must not depend on i915. i915 may be
used as a temporary observation/validation owner, but its GPU addresses,
bindings, page tables, residency decisions, or execution results must never be
reported as IDW-owned behavior. Software tests establish software invariants;
they do not establish independent GPU control.

Keep the safety distinction explicit:

- **Development safeguards remain mandatory:** source review, checked arithmetic,
  allocation and mapping lifetime rules, rollback, lock ordering, diagnostic
  state, backups, reproducible tests, and generation-specific hardware research.
- **Operational restrictions can be relaxed only in an isolated environment:**
  use a disposable system/VM and a dedicated test device before allowing IDW to
  bind the GPU. Never let IDW and i915 own the same physical GPU concurrently.
  Do not test page-table writes on the active desktop GPU.

## Gen7.5 baseline to validate

The initial Haswell implementation target is the legacy Gen6/Gen7 two-level
PPGTT family used by Linux i915 for graphics versions below 8, not the Gen8+
multi-level page-table format.

The upstream Linux v6.12 i915 source provides a reference baseline:

- [`gen6_ppgtt.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gt/gen6_ppgtt.c)
  contains common Gen6 PPGTT table handling and a distinct Gen7 enable path;
  that path has a Haswell-specific `ECOCHK_PPGTT_WB_HSW` setting.
- [`gen6_ppgtt.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gt/gen6_ppgtt.h)
  and [`intel_gtt.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/gt/intel_gtt.h)
  define the legacy table indices/geometry. The source uses 32-bit PTEs,
  4 KiB pages, a PDE shift of 22, 1024 PTEs per page table, and a 2 GiB
  address-space iteration range (512 PDE slots). The same source defines
  address encoding as `addr | ((addr >> 28) & 0xff0)`, valid bits at bit 0,
  and `HSW_PTE_UNCACHED` as zero. The software model uses this address transform
  for 40-bit DMA page addresses and only the uncached PTE policy.
- These source facts are an implementation reference, not a substitute for the
  applicable Intel PRM, Haswell SKU/stepping checks, or hardware validation.
  The current CPU model uses the source-verified address transform, valid bits,
  and uncached policy only. Before hardware use, cross-check DMA mask,
  permissions, cache policy, page-table visibility, invalidation ordering, and
  engine/context activation against the exact Haswell PRM and i915
  implementation. Do not copy Gen8+ address-width, entry formats, or
  invalidation assumptions.

## Delivery stages

These are implementation stages with explicit evidence gates. A stage is not
complete because its interface or a CPU-only model compiles.

| Stage | Milestone | Proof required | Current status |
|---|---|---|---|
| 1 | GPU resource read/write | GPU writes data into a managed resource; CPU reads back the exact result. | Passed on Haswell `8086:0412` through i915: wrote `0x2468ace0` at offset 16 and verified exact CPU readback. This is not independent execution. |
| 2 | Native DMA | IDW allocates and maps real DMA memory for its exclusively owned PCI device. | In progress: an ownership-checked `dma_alloc_coherent` page provider compiles and tracks device generation/allocation cookies; no real IDW-owned device has called it. Existing page-table tests still use synthetic descriptors. |
| 3 | Hardware PPGTT | GPU uses IDW-created Haswell Gen7.5 page tables. | Not implemented; the existing model is CPU-only, and i915 owns hardware page tables. |
| 4 | Independent submission | IDW submits and completes commands without i915 graphics execution. | Not implemented. |
| 5 | Scheduler | Multiple contexts, queues, timeout and recovery. | Not implemented independently. |
| 6 | UMD | Real shader compilation and rendering. | Not implemented. |
| 7 | Display/KMS | IDW-controlled display pipeline. | Not implemented. |
| 8 | Ownership transition | Safely detach i915 and give IDW exclusive hardware control. | Not attempted; i915 remains bound and owns the GPU. |

Stage 2 must supply valid DMA-backed page-table memory for stage 3. A GPU
mapping is not hardware-backed until the Haswell-specific publication,
synchronization, invalidation and activation requirements have completed.
The CPU PPGTT model and the stage 1 i915-assisted write/readback do not satisfy
that gate.

## PPGTT subsystem boundaries

### VidMm and GPU-VA manager

- Own allocation object lifetime, backing references, residency state, budget
  accounting, pin counts, eviction candidates, and GPU-VA free-space intervals.
- Reserve/map/unmap ranges with checked arithmetic, alignment, permissions,
  guard policy, and per-VM address-space limits.
- Keep allocation references pinned until hardware translations are invalidated
  and all in-flight users have completed.
- Treat imported dma-buf references as backing ownership only; import alone is
  not GPU mapping or residency.

### Gen7.5 page-table core

- Keep a CPU-only table model independent of Linux kernel headers and hardware
  access. The current model supplies geometry, indexing, checked software
  translation, transactional map rollback, unmap and invalidation-state tests;
  it is not evidence of GPU address translation.
- Separate Gen7.5 32-bit PTE/PDE encoding from generic VM policy. Never reuse
  Gen8+ PTE formats or four-/five-level algorithms.
- The model encodes the upstream Gen6/7 DMA-address transform and valid bits
  with Haswell's uncached PTE policy. Cacheability selection, access
  permissions, scratch mappings, and any alternative policy remain disabled
  until verified against the matching Haswell PRM.
- Use a backend-provided CPU mapping and DMA address for every page-table page.
  A CPU physical address is not a GPU DMA address.
- Treat every DMA address as scoped to the exact device and device-generation
  that produced it. Page allocations and mapped resource pages carry that scope
  plus an allocation/mapping lifetime cookie; reject cross-device or stale
  scope use before constructing entries.
- Build missing tables off to the side, validate all backing segments and
  encodings, then publish the entries with required synchronization. On error,
  restore old entries and invalidate again; if rollback/invalidation fails,
  poison the VM and reject further submissions rather than reporting success.
- For unmap, clear entries to the documented scratch/invalid encoding, perform
  the required visibility and invalidation sequence, and only then release
  backing pins or page-table memory.
- Keep cacheability alternatives, permission semantics, scratch mapping,
  physical page-directory layout/alignment, and live hardware update rules
  unimplemented until cross-checked against the exact Haswell PRM and upstream
  Gen7 path.

### Hardware backend

- Own PCI identity/stepping checks, DMA mask negotiation, coherent/non-coherent
  page-table allocation, DMA mapping/unmapping, cache synchronization, MMIO,
  engine/context activation, translation invalidation, interrupts, reset and
  power sequencing.
- Expose capability results as data. Reject unsupported page sizes, address
  widths, engines or coherency behavior instead of silently substituting.
- Provide fault-injectable callbacks for CPU tests. Test callbacks must be
  explicitly named software/fake and cannot report hardware completion.
- Enforce exclusive device ownership and a staged `probe -> initialize ->
  memory/VM -> engines/IRQ -> ready` transition with reverse-order rollback.

The i915-assisted adapter can validate GEM backing, dma-buf reference lifetime,
context VM binding, and submission/completion through supported DRM UAPI; it
cannot expose i915's device DMA addresses, private PPGTT pages, synchronization,
invalidation, or activation to IDW. Its capability profile therefore fails a
DMA-page-table request. The native DMA contract requires device-scoped pages
and mappings, synchronization, Haswell invalidation and VM activation
operations, and an exclusive-owner declaration. Its validator checks only
contract completeness; it is not an implemented DMA provider. A test platform
device is not a substitute for the GPU's DMA device, and IDW must not call the
DMA API against i915's device from an unrelated driver.

## Proposed real-backend interfaces

`include/idw/contracts/page_table_memory.h`,
`include/idw/contracts/i915_assisted.h`,
`include/idw/contracts/native_dma.h`, and
`include/idw/contracts/haswell_ppgtt.h` define the backend-neutral boundary:

- `idw_dma_page` holds a CPU mapping separately from its device-scoped DMA
  address and allocation-lifetime cookie. Mapped page addresses include a
  device scope and mapping-lifetime cookie.
- `idw_pt_memory_ops` provides page-table-page allocation/free, allocation
  DMA map/unmap, and page-table synchronization.
- `idw_gen75_ppgtt_ops` provides capability query, range invalidation, and
  per-engine VM activation/deactivation hooks.
- The i915-assisted contract reports only supported DRM/GEM validation
  capabilities. The native DMA contract requires exclusive device ownership
  and complete memory/PPGTT operations.

The device-scope validators and i915 capability profile are implemented and
tested; native contract validation is structural only. Before a runtime native
backend,
specify callback context/sleepability, ownership on partial failure, DMA
segments versus mapped pages, cache-coherency guarantees, invalidate completion
semantics, lock ordering, and the engine identifiers. Keep any engine-specific
activation implementation in the hardware/submission layer; do not make a
generic table writer issue undocumented MMIO.

The CPU-only software APIs live in `include/idw/gpuva_model.h` and
`include/idw/haswell_ppgtt_model.h`, with implementations in `src/`. Their page
provider callback is a test/software seam. The current test allocator assigns
synthetic DMA addresses, which must never be treated as a device DMA mapping.

## Required tests and gates

1. **CPU-only model:** implemented and tested for compile-time geometry;
   PTE/PDE index boundary vectors; VA zero/end/bounds tests; overlap and guard
   tests; map/unmap round trips; multi-PDE mappings; allocation-failure rollback;
   deferred table reclamation; invalidation generation and poison state. Access
   permission, allocation-refcount, and concurrent lifecycle tests remain.
2. **Failure injection:** fail each page allocation, DMA map, synchronization,
   entry publication, invalidation, activation, rollback and teardown point.
   Verify there is no success-shaped fallback, leaked backing, double unmap, or
   freed page table still reachable by hardware.
3. **Concurrency:** map/unmap/destroy versus submit/close, lockdep and KASAN,
   and delayed completion while the backing reference remains pinned.
4. **Static review:** compare constants and operations against a pinned upstream
   i915 revision and the matching Haswell PRM. Upstream geometry and uncached
   address/valid encoding are used by the CPU model; resolve remaining PRM
   questions before enabling hardware bit encodings or access policies.
5. **Emulation:** use only a platform that actually models the required Intel
   graphics generation and behavior. Generic QEMU display devices do not model
   Haswell PPGTT. A software simulator validates software invariants only.
6. **Physical hardware:** use an isolated disposable Haswell test system, known
   recovery media, backups, serial/remote recovery, and a documented exclusive
   ownership handoff. Never run experimental page-table programming while i915
   is bound or on the active desktop GPU.

Only stage-specific evidence changes implementation status. A passing CPU model
is software evidence; it does not prove a hardware page walk, translation
invalidation, independent execution, or safe device ownership.
