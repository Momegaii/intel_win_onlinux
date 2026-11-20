# Haswell Gen7.5 physical PPGTT readiness

Status: **software model corrected; physical implementation not started.** This
document records an audit of the Gen7.5 PPGTT model against the Intel Haswell
Programmer's Reference Manual (PRM) and the Linux v6.12 i915 Gen6/Gen7
implementation, defines the physical page-directory/page-table requirements,
and specifies a minimal reversible page-walk experiment for a future
exclusively owned device. The CPU-side encoding, GGTT-directory, and deferred
reclamation findings from the audit have been implemented and tested.

No hardware state was changed while preparing this document or implementing
the corrected model. i915 remains bound, owns the GPU, and was not unbound. No
PPGTT register was programmed, no page table was published to hardware, no TLB
invalidation was issued, and no physical GPU page walk is claimed.

## 1. Scope and non-goals

In scope:

- Audit the existing two-level Haswell Gen7.5 PPGTT software model and DMA
  storage adapter against authoritative documentation.
- State the physical requirements for page directories, page tables, entries,
  alignment, address width, cache coherency, and invalidation.
- Identify the Haswell registers involved and their current i915 ownership.
- Identify what can be validated without taking ownership from i915.
- Design a minimal, reversible physical page-walk experiment with
  prerequisites, observations, abort criteria, and recovery.
- Separate documentation facts, software-test facts, QEMU facts, and physical
  hardware facts.
- Update the implementation plan and project state.

Out of scope for this milestone:

- Unbinding i915 or binding IDW to the physical GPU.
- Programming `PP_DCLV`, `PP_DIR_BASE`, `RING_MODE_GEN7`, `INSTPM`, or any
  other PPGTT/engine register.
- Allocating GGTT space, publishing PDEs/PTEs, invalidating hardware
  translations, or activating a VM.
- Claiming a physical Haswell page walk, hardware translation, or independent
  GPU execution.
- Implementing the experiment. This document stops at a reviewed plan.

## 2. Evidence levels used here

Every statement below is tagged by the strongest evidence available:

| Level | Meaning |
|---|---|
| **D** | Stated by the Intel Haswell PRM or by the Linux i915 source. This is documentation/source evidence, not silicon evidence. |
| **S** | Established by a software unit test or software model in this repository. |
| **Q** | Established under QEMU with a virtual device. QEMU does not model Haswell PPGTT. |
| **H** | Established on physical Haswell hardware. |

The current milestone has **D** and **S** evidence, plus existing **Q** evidence
for page-table storage. It has **no new H evidence** and does not convert old
i915-assisted Stage 1 results into IDW PPGTT evidence.

## 3. Authoritative references

Intel documentation:

- Intel, *Haswell Programmer's Reference Manual, Volume 5: Memory Views*,
  sections "Graphics Translation Tables", "Per Process GTT", "Page Table
  Format", "Page Walk", "PPGTT Directory Entries (PDEs)", and "PPGTT Table
  Entries (PTEs)". Original path:
  `https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-hsw-memory-views.pdf`.
  Archived copy used for this audit:
  `https://web.archive.org/web/2020id_/https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-hsw-memory-views.pdf`.
- Intel, *Haswell Programmer's Reference Manual, Volume 2c: Command Reference:
  Registers*, sections `PP_DCLV`, `GFX_MODE`, `INSTPM`, and `ECOCHK`. Original
  path:
  `https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-hsw-commandreference-registers_0_0.pdf`.
  Archived copy used for this audit:
  `https://web.archive.org/web/2020id_/https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-hsw-commandreference-registers_0_0.pdf`.
- Intel, *Haswell Programmer's Reference Manual, Volume 2c: Command Reference:
  Instructions*, `MI_STORE_DATA_IMM` bit 22 ("Use Global GTT") and its PPGTT
  selection rules. Archived copy used for this audit:
  `https://web.archive.org/web/2020id_/https://01.org/sites/default/files/documentation/intel-gfx-prm-osrc-hsw-commandreference-instructions_0_0.pdf`.

Linux i915 reference, pinned to v6.12:

- `drivers/gpu/drm/i915/gt/gen6_ppgtt.c`
- `drivers/gpu/drm/i915/gt/gen6_ppgtt.h`
- `drivers/gpu/drm/i915/gt/intel_gtt.h`
- `drivers/gpu/drm/i915/gt/intel_ring_submission.c`
- `drivers/gpu/drm/i915/gt/intel_ggtt.c`
- `drivers/gpu/drm/i915/gt/intel_gt_regs.h`
- `drivers/gpu/drm/i915/gt/intel_engine_regs.h`
- `drivers/gpu/drm/i915/i915_reg.h`

Repository references:

- `include/idw/haswell_ppgtt_format.h`
- `include/idw/haswell_ppgtt_model.h`
- `src/haswell_ppgtt_model.c`
- `include/idw/contracts/haswell_ppgtt.h`
- `include/idw/contracts/page_table_memory.h`
- `include/idw/pt_dma_adapter.h`
- `src/pt_dma_adapter.c`
- `kernel/idw_dma_pt_adapter.c`
- `tests/ppgtt-model-test.c`
- `tests/pt-dma-adapter-test.c`
- `tests/qemu-dma/evidence-2026-11-20-tcg-page-table-corrected.log`

The i915 source is an implementation reference, not a substitute for the PRM.
Where the PRM and i915 differ, both are recorded and the difference is treated
as an open question.

## 4. Audit of the current implementation

### 4.1 What matches the documentation

The current model uses the Gen6/Gen7 two-level layout:

- 4 KiB pages.
- 1024 32-bit PTEs per page table.
- PDE shift 22 (each PDE covers 4 MiB).
- 512 PDE slots in the 2 GiB profile, with a 31-bit GPU virtual address.
- Bit 0 valid in both PDEs and PTEs.
- A 4 KiB page-table page supplied by the DMA storage adapter.
- Device/generation scope and allocation cookies on DMA pages.
- Deferred reclamation: unmap keeps the PDE valid; quiesced reclaim points the
  PDE at a zeroed scratch page table, and the old table is freed only after the
  resulting invalidation generation is acknowledged.

These are consistent with the PRM's "Gen6-Gen7.5 family ... 2-level mapping
scheme" and with i915's `I915_PDES`, `GEN6_PDE_SHIFT`, and `GEN6_PTES`
definitions. They are software/table-format facts, not a hardware page walk.

### 4.2 Audit findings and software disposition

The audit found correctness problems in the CPU-side model. They were fixed in
the follow-up implementation; the table records the pre-correction state and
the disposition. None of these fixes touched hardware.

| Finding | Pre-correction implementation | Authoritative requirement | Software disposition |
|---|---|---|---|
| PDE and PTE address encoders were shared | `idw_hsw_ppgtt_encode_address()` used `addr | ((addr >> 28) & 0xff0)` for both PDEs and PTEs. | PRM: PDE bits 11:4 carry physical address bits 39:32. PTE bits 10:4 carry physical address bits 38:32; PTE bit 11 is cacheability control bit 3. | **Corrected.** Separate `idw_hsw_ppgtt_encode_pde()` and `idw_hsw_ppgtt_encode_pte()` helpers use `0xff0` and `0x7f0` respectively. Golden vectors cover upper address bits and PTE bit 11. |
| Mapped pages accepted 40-bit addresses | `IDW_HSW_PPGTT_DMA_ADDRESS_BITS` was 40 for mapped pages. | PRM: mapped pages are limited to 39-bit physical addresses because PTE bit 39 is repurposed. PDEs still carry bits 39:32. | **Corrected.** Page tables use `< 2^40`; mapped pages use `< 2^39`. Both reject misalignment and overflow separately. |
| Page directory was a CPU array | The model kept `uint32_t directory[512]` in CPU memory. | PRM: the directory must reside in the GGTT, and PDEs can only be modified through GTTADDR. | **Corrected in the model.** A directory provider contract supplies a GGTT-backed 4 KiB directory; the model keeps a separate CPU shadow and performs PDE writes/reads through GTTADDR callbacks. |
| PDE removal was treated as ordinary unmap | `idw_hsw_ppgtt_unmap()` cleared the PDE, retired the table, and freed it after invalidation acknowledgement. | PRM: a running context caches directory entries and has no on-the-fly PDE invalidation. i915 keeps the PDE valid and clears PTEs to a scratch entry. | **Corrected.** Unmap keeps the PDE valid. `idw_hsw_ppgtt_reclaim_empty()` requires quiescence and no pending invalidation, points the PDE at a zeroed scratch table, and defers freeing the old table until the next invalidation completes. |
| Invalidation was only a software generation | The model tracked a dirty range and generation. | PRM/i915 require TLB invalidate plus sync flush; PDE updates require context-inactive rules. | **Partially corrected in the model.** The model now tracks active/quiesced state and rejects PDE publication or reclamation while active. Register-level invalidation remains absent and i915-owned. |
| No PPGTT activation path | The model has no `PP_DCLV`, `PP_DIR_BASE`, `RING_MODE_GEN7`, or `INSTPM` operations. | i915 programs `PP_DCLV` then `PP_DIR_BASE`, enables PPGTT per engine, and invalidates TLBs before submitting work. | **Unchanged; physical blocker.** VM activation and engine/context ownership are missing. |
| Cacheability policy is only PTE UC | PTE valid plus uncached (`0`) is modeled. | PRM provides 4-bit cacheability controls; i915 also sets global `ECOCHK` PPGTT override and `GAC_ECO_BITS` for Haswell. | **Unchanged; physical blocker.** The PTE UC choice is valid, but global walk/cache policy is not modeled or verified. |
| Alignment is only 4 KiB for PT and pages | Adapter/model require 4 KiB alignment. | PRM: translation table bases are 4 KiB aligned; i915 pins the Gen6 PD VMA with `GEN6_PD_ALIGN = 64 KiB`. | **Modeled.** Directory provider requires at least 4 KiB GGTT alignment and exposes i915's 64 KiB recommendation as a constant. The hardware requirement remains open. |
| 32 KiB pages are unsupported | PDE bit 1 is always zero. | PRM: PDE bit 1 selects 4 KiB or 32 KiB pages. | **Rejected explicitly.** `idw_hsw_ppgtt_validate_pde_entry()` returns `-EOPNOTSUPP` for the 32 KiB bit instead of silently accepting it. |

### 4.3 Before/after encoding examples

| Case | Before (shared encoder) | After |
|---|---|---|
| PDE for page table at `0x12_3456_7000` | `0x34567121` | `0x34567121` (unchanged; PDE bits 11:4 are address bits) |
| PTE for page at `0x12_3456_7000`, UC | `0x34567121` | `0x34567121` (unchanged below 2^39) |
| PDE for page table at `0x80_0000_0000` | `0x00000801` | `0x00000801` (valid 40-bit page-table address) |
| PTE for page at `0x80_0000_0000` | `0x00000801` (address bit 39 placed in cacheability bit 11) | `-ERANGE` (mapped pages must be below 2^39) |
| PTE for page at `0`, cacheability `0x8` | `0x00000001` (cacheability ignored) | `0x00000801` (bit 3 of cacheability maps to entry bit 11) |
| PTE for maximum page at `0x7f_ffff_f000`, UC | `0xfffff7f1` | `0xfffff7f1` |
| PDE with reserved bit 2 set | accepted | `-EINVAL` |
| PDE with 32 KiB page-size bit set | accepted | `-EOPNOTSUPP` |

### 4.4 Audit conclusion

The CPU-side correctness blockers are fixed: separate PDE/PTE encoders,
distinct 40-bit/39-bit limits, GGTT-resident directory modeling with GTTADDR
writes, and scratch-table retention with deferred reclamation. The model is
still not a hardware-publication implementation. Remaining physical blockers
are a hardware GGTT aperture allocator and MMIO GTTADDR path, register ownership, TLB/GGTT
invalidation, VM activation, cache-policy selection, coherency/IOMMU behavior,
and physical evidence.

## 5. Physical page-directory and page-table requirements

### 5.1 Geometry

| Property | Value | Evidence |
|---|---|---|
| Translation scheme | Two levels: page directory, then page table | D (PRM "Per Process GTT") |
| Page size | 4 KiB for the first experiment | D (PRM "Page Table Format") |
| Page directory size | 4 KiB, 512 active PDEs in the 2 GiB profile | D + i915 `I915_PDES=512`, `PP_DIR_DCLV_2G` |
| Page table size | 4 KiB, 1024 32-bit PTEs | D + i915 `GEN6_PTES` |
| PDE coverage | 4 MiB per PDE (`1 << 22`) | D + i915 `GEN6_PDE_SHIFT=22` |
| PT coverage | 4 MiB per page table (1024 x 4 KiB) | D |
| GPU VA in the 2 GiB profile | Bits 31:22 PDE index, 21:12 PTE index, 11:0 offset | D + i915 |
| 2 GiB profile | 512 PDEs x 4 MiB = 2 GiB | i915 `PP_DIR_DCLV_2G=0xffffffff` |

The PRM also says each 4 KiB directory page contains 1024 DWORD entries. The
2 GiB profile used by i915 enables 512 of them. Whether Haswell exposes a
larger per-process VA profile, and how `PGTBL_CTL2` controls it, remains an
open question.

### 5.2 Page directory (PDE) format

A PDE is one 32-bit DWORD per 4 KiB page table, covering 4 MiB of GPU VA.

| Bits | Field | Requirement |
|---|---|---|
| 31:12 | Physical page address 31:12 | Page-table base address bits 31:12. |
| 11:4 | Physical page address 39:32 | Page-table base address bits 39:32. |
| 3:2 | Reserved | Must be zero. |
| 1 | Page size | `0` = 4 KiB pages; `1` = 32 KiB pages. |
| 0 | Valid | `1` = valid; `0` = invalid and causes a page fault on access. |

PDE encoding for a 4 KiB page table:

```c
pde = (uint32_t)(pt_dma & 0xfffff000u) |
      (uint32_t)((pt_dma >> 28) & 0x00000ff0u) |
      1u; /* valid, 4 KiB pages */
```

The page table base must be 4 KiB aligned. The PDE can carry a 40-bit page-table
address because bits 39:32 are address bits, not cacheability bits.

### 5.3 Page table (PTE) format

A PTE is one 32-bit DWORD per 4 KiB graphics page.

| Bits | Field | Requirement |
|---|---|---|
| 31:12 | Physical page address 31:12 | Mapped page base address bits 31:12. |
| 11 | Cacheability control [3] | High bit of the 4-bit cacheability control. |
| 10:4 | Physical page address 38:32 | Mapped page base address bits 38:32. |
| 3:1 | Cacheability control [2:0] | Low three bits of the cacheability control. |
| 0 | Valid | `1` = valid; `0` = invalid and causes a page-table error on access. |

PTE encoding for a 4 KiB uncached page:

```c
pte = (uint32_t)(page_dma & 0xfffff000u) |
      (uint32_t)((page_dma >> 28) & 0x000007f0u) |
      0u | /* cacheability 0000: UC */
      1u;  /* valid */
```

The mapped page must be 4 KiB aligned and below 2^39. The PRM states that
Haswell repurposes physical address bit 39 as cacheability control, because
client systems are only required to support 39-bit physical addressing. Do not
use the PDE `0xff0` transform for PTEs.

### 5.4 Cacheability

The PTE's 4-bit cacheability control selects the memory type, cache level, and
aging policy for the mapped page. The PRM table includes:

| Control | Meaning |
|---|---|
| `0000` | UC in LLC/eLLC; allocation age don't care. |
| `0010` / `0011` | WB in LLC only, age 3 / age 0. |
| `0111` / `0110` | WT in LLC/eLLC, age 3 / age 0. |
| `1000`-`1011` | WB in LLC/eLLC, ages 3-0. |
| `1100`-`1111` | WB in eLLC (UC in LLC), ages 3-0. |

The current model uses `0000` (UC). That is a valid conservative choice for a
first experiment. It is not a complete cache-policy implementation.

Haswell also has global PPGTT cacheability controls:

- `ECOCHK` (`0x4090`) bits 4:3: `00` no override, `01` UC, `10` WT, `11` WB.
  i915's Haswell path selects WB (`ECOCHK_PPGTT_WB_HSW`, `3 << 3`).
- `GAC_ECO_BITS` (`0x14090`) bits 9:8: i915 selects 64-byte PPGTT cache
  behavior (`ECOBITS_PPGTT_CACHE64B`, `3 << 8`).

The exact scope of the `ECOCHK` override (page-table walks, data accesses, or
both) and whether it must match the PTE policy are open questions. A physical
implementation must not silently choose a different policy from the one it
validated.

### 5.5 Cache coherency

The PRM requires:

- Page tables reside in main memory and are **not snooped**.
- Page directories reside in the GGTT and are accessed through GTTADDR.
- Page tables may be updated directly in memory "if proper precautions are
  taken", or from the command stream with `MI_UPDATE_GTT`.

Practical requirements:

1. **Page tables:** use coherent DMA memory, or streaming DMA memory with an
   explicit `dma_sync_single_for_device()` (and CPU cache flush where required)
   before the GPU walks the table. Do not leave dirty CPU cache lines that the
   non-snooped GPU cannot see.
2. **PDEs:** write through the GGTT aperture, not through an arbitrary CPU
   mapping of the PD backing page. Use uncached MMIO writes, a write barrier,
   and a GGTT flush/posting read. The PRM says PDEs "can only be modified using
   GTTADDR".
3. **Mapped pages:** if they are streaming rather than coherent, synchronize
   before GPU reads and after GPU writes.
4. **IOMMU:** page-table entries hold device DMA addresses. With an IOMMU, those
   are IOVAs translated by the IOMMU, not necessarily CPU physical addresses.
   The PRM's "physical address" wording and the DMA API's IOVA must be
   reconciled and tested on the target configuration.

### 5.6 Invalidation

PTE changes:

1. Write the PTE.
2. Make the write visible to the device (`dma_wmb()` and/or DMA sync).
3. Invalidate the engine TLB with `INSTPM` bit 9 (TLB invalidate) and bit 5
   (sync flush), then poll until the sync-flush bit clears.
4. i915 also uses `GFX_FLSH_CNTL_GEN6` (`0x101008`, bit 0) for GGTT/PDE
   visibility and a posting read.
5. The PRM notes that GFX soft resets do not invalidate TLBs; software must
   invalidate explicitly after reset.

PDE changes:

- Update only when the context is not active.
- A running context caches directory entries; the PRM says it is unknown when
  an update will take effect.
- `PP_DCLV` controls which 16-entry groups are loaded on context restore.
  i915 writes `0xffffffff` for the 2 GiB profile.
- `PP_DIR_BASE` is a global context register, not maintained per context in
  ring-buffer mode. i915 loads `PP_DCLV` then `PP_DIR_BASE` through
  `MI_LOAD_REGISTER_IMM` before submitting a context, after ensuring the pipe is
  flushed and TLBs are invalidated.
- Do not free a page table while a cached PDE may still reference it. Use a
  scratch page table, or destroy/recreate the context.

## 6. Registers and i915 ownership

All registers below are owned by i915 while it is bound. None were touched in
this investigation.

| Register | Offset | Purpose | i915 use | IDW status |
|---|---:|---|---|---|
| `PP_DCLV` | `0x2220` (64-bit) | PPGTT directory cacheline valid; bits 31:0 enable 16-entry groups | Writes `PP_DIR_DCLV_2G` (`0xffffffff`) | Not owned; no write path |
| `PP_DIR_BASE` | `0x2228` | Page-directory base offset in GGTT | Writes per-engine `pp_dir` byte offset | Not owned; no hardware GGTT allocator |
| `GFX_MODE` / `RING_MODE_GEN7` | `0x229c` (render; per-engine base + `0x29c`) | Per-process GTT enable, bit 9 | Sets `GFX_PPGTT_ENABLE` for Gen7+ | Not owned |
| `INSTPM` | `0x20c0` (render; per-engine base + `0xc0`) | TLB invalidate bit 9, sync flush bit 5 | Uses both for engine TLB invalidation | Not owned |
| `GFX_FLSH_CNTL_GEN6` | `0x101008` | GGTT flush, bit 0 | Used by `gen6_ggtt_invalidate()` | Not owned |
| `ECOCHK` | `0x4090` | PPGTT cacheability override, bits 4:3 | Selects WB for Haswell | Not owned |
| `GAC_ECO_BITS` | `0x14090` | PPGTT cache behavior, bits 9:8 | Selects 64-byte cache behavior | Not owned |
| `PGTBL_CTL` | `0x2020` | Global GTT base | i915 owns GGTT | Not owned |
| `PGTBL_CTL2` | Not found in the HSW register volume | PRM says it controls PPGTT base/size | Gen6/Gen7 i915 path uses `PP_DIR_BASE`/`PP_DCLV`, not this register | Open question |
| `RING_FAULT_REG` / `PP_PFD` | Per-engine / `PP_PFD` range | Page-fault diagnostics | i915 owns | Read-only diagnostics only after ownership |

Note on naming: the Haswell PRM calls the per-engine register at `0x229c`
`GFX_MODE`. i915 names it `RING_MODE_GEN7`. i915's `GFX_MODE` macro at `0x2520`
is a different register and must not be substituted for the PPGTT enable bit.

i915's Gen7.5 sequence, for reference:

1. `gen7_ppgtt_enable()`: set `GAC_ECO_BITS` PPGTT cache 64B; set
   `ECOCHK` PPGTT WB for Haswell.
2. `set_pp_dir()`: write `RING_PP_DIR_DCLV = PP_DIR_DCLV_2G`; write
   `RING_PP_DIR_BASE = pp_dir`; set `RING_MODE_GEN7` bit 9.
3. `flush_cs_tlb()`: write `RING_INSTPM` with TLB invalidate and sync flush;
   poll sync flush clear.
4. `gen6_flush_pd()`: write PDEs through `pd_addr`; `mb()`; posting read;
   `gen6_ggtt_invalidate()`; `mb()`.
5. `load_pd_dir()`: load `PP_DCLV`, `PP_DIR_BASE`, then set
   `INSTPM_TLB_INVALIDATE`.

IDW would need equivalent operations, but only after exclusive ownership and
with saved/restored register state. None of this is implemented.

## 7. What can be validated safely while i915 remains bound

Safe, read-only validation:

- `lspci -nnk -s 00:02.0` and sysfs driver links to confirm `8086:0412` and
  i915 ownership.
- `dmesg` and i915 version/configuration output to confirm PPGTT is enabled and
  which GPU stepping is present.
- Read-only i915 debugfs/sysfs telemetry where the running kernel exposes it.
  Do not depend on private i915 structures or write to debugfs.
- The existing i915-assisted Stage 1 resource write/readback and memory
  lifecycle tests. These are physical GPU evidence for i915's mappings, not for
  IDW PPGTT.
- The existing CPU PPGTT model and DMA adapter tests.
- The existing QEMU EDU page-table storage test.

Not safe while i915 is bound:

- Any write to `PP_DCLV`, `PP_DIR_BASE`, `RING_MODE_GEN7`, `INSTPM`,
  `GFX_FLSH_CNTL_GEN6`, `ECOCHK`, `GAC_ECO_BITS`, `PGTBL_CTL`, or engine/ring
  registers.
- Allocating GGTT space or writing PDEs.
- Creating an IDW-owned context or activating an IDW VM on a physical engine.
- Unbinding i915, reloading i915, or using `/dev/mem` to poke GPU registers.
- Treating an i915-assisted translation as an IDW page walk.

A passive read of i915's page tables is not part of this plan. It would require
a stable public interface or a private-structure dependency, and neither is
justified for the first experiment.

## 8. Minimal reversible physical page-walk experiment

This is a design only. It must not be executed until the prerequisites and
review gates are satisfied.

### 8.1 Goal

Prove that the Haswell page walker reads an IDW-created PDE from a GGTT-resident
page directory, reads an IDW-created PTE from a DMA-backed page table, and
translates a GPU virtual address to a data page that the GPU then writes.

The first experiment deliberately uses one PDE, one page table, one valid PTE,
one engine, one ring command, and 4 KiB pages. PTE 0 is left invalid. It does
not test context switching, live PDE replacement, 32 KiB pages, instruction
fetch through PPGTT, or multiple VMs.

### 8.2 Prerequisites

- A disposable Haswell test system or a dedicated GPU that can be recovered
  independently. Do not use the active desktop GPU.
- i915 unbound or never bound for the experiment; IDW has exclusive PCI
  ownership. This is a future ownership transition, not something to do now.
- A kernel module with PCI BAR mapping, forcewake/power-well control, GPU reset,
  engine/ring setup, interrupt or polling completion, and register save/restore.
- A GGTT allocator and GTTADDR mapping for the page directory.
- Coherent DMA page allocation for the page table and data page, with device
  scope, 4 KiB alignment, and address-width enforcement.
- A known-good recovery path: serial console or SSH, watchdog, reboot/power
  cycle, and a way to re-enable i915.
- Saved original values for every touched register, and a tested restore path.
- A build that can reject unsupported address widths, alignments, and page
  sizes before touching hardware.

### 8.3 Minimal topology

| Object | GPU VA / location | Requirement |
|---|---|---|
| Page directory | GGTT offset, 4 KiB aligned (prefer 64 KiB) | 512 PDEs; only PDE 64 used |
| Page table | DMA page, 4 KiB aligned, below 2^40 | 1024 PTEs; PTE 1 used, PTE 0 invalid |
| Data page | DMA page, 4 KiB aligned, below 2^39 | Receives the GPU store |
| GPU VA for data | `0x10001000` | PDE index 64, PTE index 1 |
| Command | Ring `MI_STORE_DATA_IMM` to `0x10001000` | Bit 22 ("Use Global GTT") clear selects PPGTT |

PDE 64 covers GPU VA `0x10000000`-`0x103fffff`. The data page is the second 4 KiB
page in that range. A single page table is sufficient.

### 8.4 Setup

1. Allocate and zero the page directory in GGTT. Record its GGTT byte offset.
2. Allocate and zero the page table and data page in coherent DMA memory.
   Fill the data page with a known non-magic value.
3. Write PDE 64 with the page-table DMA address, 4 KiB page-size bit clear,
   and valid set.
4. Write PTE 1 with the data-page DMA address, cacheability `0000` (UC), and
   valid set. Leave PTE 0 invalid for this experiment.
5. Synchronize the page table for the device.
6. Write the PDE through GTTADDR with 32-bit uncached writes, then issue a GGTT
   flush/posting read.
7. Read back the PDE and PTE through the CPU mapping and verify the exact
   expected values before enabling PPGTT.

### 8.5 Activation and command

1. Ensure the engine is idle.
2. Save `PP_DCLV`, `PP_DIR_BASE`, `RING_MODE_GEN7`, `INSTPM`,
   `GFX_FLSH_CNTL_GEN6`, `ECOCHK`, and `GAC_ECO_BITS`.
3. Program `PP_DCLV = 0xffffffff` for the 2 GiB profile.
4. Program `PP_DIR_BASE` with the page-directory GGTT byte offset.
5. Set the per-engine PPGTT enable bit (`RING_MODE_GEN7` bit 9).
6. Issue the TLB invalidate plus sync flush, and wait for completion.
7. Submit one ring command that stores a known magic value to `0x10001000`
   through the PPGTT address space. Leave `MI_STORE_DATA_IMM` bit 22 ("Use
   Global GTT") clear; the PRM says bit 22 clear selects PPGTT. Do not use this
   command from a non-secure batch buffer to reach global GTT.
8. Wait for ring completion with a bounded timeout.

### 8.6 Expected observations

- PDE readback equals the expected encoded page-table address plus valid.
- PTE readback equals the expected encoded data-page address plus valid.
- `PP_DIR_BASE` readback equals the page-directory GGTT offset.
- The per-engine PPGTT enable bit reads back set.
- No page fault is reported for the data access.
- The ring command completes within the timeout.
- CPU readback of the data page returns the exact magic value.
- After teardown and reboot, i915 rebinds and the existing i915-assisted Stage 1
  test still passes.

### 8.7 Teardown and recovery

Normal teardown:

1. Stop the ring and wait for idle.
2. Restore saved registers in reverse order; clear the PPGTT enable bit.
3. Flush and posting-read the affected registers.
4. Free the GGTT page directory, page table, and data page.
5. Reset the engine/GPU if required.
6. Unload IDW, re-enable i915, or reboot.
7. Verify i915 rebind and baseline health.

Failure recovery:

- Any page fault, GPU hang, timeout, unexpected register readback, display
  corruption, or machine instability: stop immediately.
- Attempt engine reset, then GPU reset, then restore saved registers.
- If reset does not recover, reboot or power-cycle.
- Do not attempt to "retry around" a fault on the active machine.
- Preserve the serial log and register dump for analysis.

### 8.8 Abort criteria

Abort before enabling PPGTT if any of these is true:

- Page directory is not in GGTT or not 4 KiB aligned.
- Page table DMA address is not 4 KiB aligned or is not below 2^40.
- Data page DMA address is not 4 KiB aligned or is not below 2^39.
- Device scope, generation, or allocation cookie mismatch.
- PDE/PTE readback does not match the expected encoding.
- Required register save/restore or reset path is unavailable.
- i915 is still bound or any other driver owns the GPU.

### 8.9 Optional negative control

After the positive experiment passes on a disposable system, a separate run may
clear the PTE valid bit and confirm a page fault. This is optional and riskier:
a fault may hang the engine. It must not be combined with the positive run and
must have the same recovery path.

### 8.10 What the experiment would and would not prove

Would prove, if it passes:

- The GPU page walker reads an IDW-created PDE from a GGTT-resident page
  directory.
- The GPU page walker reads an IDW-created PTE from a DMA-backed page table.
- A GPU data access uses IDW-created translation.
- The basic PTE/PDE encodings, alignment, and UC policy are accepted by
  Haswell.

Would not prove:

- IDW-owned instruction fetch through PPGTT.
- Live PDE replacement or directory-cache invalidation.
- Context switching, scheduling, or multiple VMs.
- 32 KiB pages, cacheability alternatives, or permission semantics.
- Independent submission, display/KMS, or production ownership.
- That the result generalizes to another Haswell SKU or stepping.

## 9. Evidence matrix

| Claim | Documentation | Software tests | QEMU | Physical hardware |
|---|---|---|---|---|
| Gen6-Gen7.5 uses a two-level PPGTT | Yes (PRM) | Yes (model geometry) | Not modeled | Not run |
| 4 KiB page tables, 1024 PTEs, PDE shift 22 | Yes (PRM + i915) | Yes | Page-table bytes copied | Not run |
| 512 PDEs / 2 GiB profile | i915; PRM says 1024 entries per page | Yes | Not modeled | Not run |
| PDE format (bits 39:32 at 11:4) | Yes (PRM) | Yes; separate PDE encoder and golden vectors | Bytes copied; independent expected values | Not run |
| PTE format (bits 38:32 at 10:4; bit 11 cacheability) | Yes (PRM) | Yes; separate PTE encoder, cacheability vector, and golden vectors | Bytes copied; independent expected values and bit-11 vector | Not run |
| Mapped pages limited to 39-bit addresses | Yes (PRM) | Yes; model and encoder reject `>= 2^39` | Uses a below-2^39 address | Not run |
| Page tables may use 40-bit addresses | Yes (PRM) | Yes; model and PDE encoder reject `>= 2^40` | Not discriminating | Not run |
| Page directory must be in GGTT | Yes (PRM) | Modeled with a directory provider and GTTADDR callbacks | Not modeled | Not run |
| PDE writes use GTTADDR | Yes (PRM) | Yes; provider callbacks write/read back through GTTADDR | Not modeled | Not run |
| CPU shadow is separate from hardware directory | Yes (PRM) | Yes; shadow accessor and hardware readback tested | Not modeled | Not run |
| Page tables in non-snooped main memory | Yes (PRM) | Coherent/streaming adapter checks | Coherent EDU pages | Not run |
| Live PDE removal is unsafe | Yes (PRM + i915) | Yes; unmap keeps PDE, active reclaim rejected, deferred free | Not modeled | Not run |
| PTE TLB invalidation via INSTPM + sync flush | Yes (PRM + i915) | Software generation only; no register access | Not modeled | Not run |
| GGTT flush via GFX_FLSH_CNTL_GEN6 | i915 | No | Not modeled | Not run |
| ECOCHK/GAC_ECO_BITS PPGTT policy | PRM/i915 | No | Not modeled | Not run |
| Page-walk translation of a data page | PRM | Software translation only | No | Not run |
| Instruction fetch through PPGTT | PRM | No | No | Not run |

## 10. Implementation plan

### Phase A — Correct the software model and contracts (complete)

- [x] Split PDE and PTE address encoders.
- [x] Enforce page-table DMA addresses `< 2^40` and mapped-page DMA addresses
  `< 2^39`.
- [x] Add golden vectors for address bit 39, PTE cacheability bit 11, PDE bit
  11, the 4 KiB page-size bit, misalignment, and overflow.
- [x] Reject 32 KiB PDEs explicitly until implemented.
- [x] Replace live PDE removal with scratch-page-table retention and require
  context quiescence before reclamation.
- [x] Separate the model's CPU shadow from GGTT-visible directory storage.
- [x] Add a page-directory-in-GGTT provider contract with allocation, GTTADDR
  write/read, flush, and release.
- [x] Add active/quiesced state and reject PDE publication or reclamation when
  quiescence cannot be established.

### Phase B — Software and QEMU validation (complete for the model)

- [x] Unit-test corrected encoders, distinct limits, and hand-derived golden
  vectors.
- [x] Unit-test scratch-table retention, active-PDE rejection, deferred
  reclamation, and no-free-while-PDE-cached behavior.
- [x] Unit-test rollback when PDE publication fails.
- [x] Run ASan/UBSan on the PPGTT and adapter tests.
- [x] Re-run the QEMU EDU page-table test with corrected independent expected
  values; keep it as storage evidence only.
- [x] Add a software simulator for the register sequence; it is labeled
  simulated.

### Phase C — Exclusive-ownership prerequisites

Software infrastructure completed in Stage 3A; real hardware prerequisites
remain open.

- [ ] Implement PCI ownership, BAR mapping, forcewake/power wells, GPU reset,
  and engine/ring setup.
- [x] Implement a checked GGTT allocator and a GTTADDR mapping abstraction.
  The allocator and hardware abstraction are tested with a simulated backend;
  no real MMIO or aperture ownership exists.
- [x] Implement register save/restore and a tested activation/deactivation
  rollback path in the hardware abstraction. This is software-state evidence
  only.
- [ ] Implement coherent DMA page allocation with the corrected limits in the
  real independent-device path.
- [ ] Prove all of this on a disposable system before any PPGTT write.

See `docs/design/HASWELL_GEN75_STAGE3A_READINESS.md`.

### Phase D — Minimal physical page walk

- Execute the experiment in section 8 on a disposable Haswell system.
- Preserve the serial log, register dump, PDE/PTE readback, and CPU data
  readback.
- Repeat after a cold boot to confirm reversibility.
- Only then consider context switching, live PDE replacement, 32 KiB pages, and
  instruction fetch through PPGTT.

### Phase E — Review and status update

- Update `docs/design/HASWELL_GEN75_PPGTT_PLAN.md`,
  `docs/IMPLEMENTATION_STATUS.md`, `PROJECT_STATE.txt`, and `SESSION_LOG.md`
  with actual results.
- Do not mark Stage 3 complete until the GPU uses IDW-created page tables and
  the evidence is preserved.

## 11. Unresolved questions

1. **`PGTBL_CTL2`:** The PRM says the PPGTT base and size are controlled by
   `PGTBL_CTL2`, but the Haswell register volume and the i915 Gen6/Gen7 path do
   not use it. What is its address, and is it relevant to Gen7.5 PPGTT?
2. **Maximum VA:** The PRM says each directory page has 1024 entries; i915
   enables 512 PDEs and 2 GiB. Does Haswell support a 4 GiB per-process profile,
   or is 2 GiB the practical limit?
3. **PD alignment:** The PRM says translation table bases are 4 KiB aligned;
   i915 uses `GEN6_PD_ALIGN = 64 KiB`. Is 64 KiB a hardware requirement, a
   PDE-cacheline requirement, or an i915 policy?
4. **PDE cache invalidation:** Exactly which operations invalidate the
   directory cache, and when is a PDE update visible to a non-active context?
5. **PTE invalidation scope:** Which of `INSTPM` TLB invalidate and
   `GFX_FLSH_CNTL_GEN6` covers PPGTT PTEs, PDEs, and GGTT entries, and what is
   the required ordering?
6. **`ECOCHK` scope:** Does `ECOCHK[4:3]` override PTE cacheability, page-table
   walk cacheability, or both? Must IDW match i915's WB setting?
7. **Coherency:** Is `dma_alloc_coherent` sufficient for non-snooped page-table
   walks on Haswell, or is an explicit cache flush required? How does VT-d/IOMMU
   change the address and coherency rules?
8. **Address width:** Confirm on the target stepping that PDEs accept 40-bit
   page-table addresses while PTEs accept only 39-bit mapped-page addresses.
9. **GGTT ownership:** How does IDW obtain GGTT space without i915, and what is
   the maximum safe PD offset?
10. **Page sizes:** Is 32 KiB PDE support required for the first experiment, or
    can it be rejected explicitly?
11. **Reset/recovery:** What is the reliable GPU reset path on Haswell iGPU, and
    does it invalidate TLBs or require explicit post-reset invalidation?
12. **Fault diagnostics:** Which fault registers are safe to read after a
    failure, and do reads clear state?
13. **Full PPGTT support:** Confirm that the target Haswell GT2 configuration
    supports full PPGTT rather than aliasing-only PPGTT.
14. **Page-table memory type:** Can page tables live in WC/UC memory, or must
    they be WB coherent? The PRM says non-snooped main memory; the practical
    mapping type needs testing.

## 12. Review checklist

This plan is ready for implementation review only when all items are checked:

- [ ] PRM revision and Haswell SKU/stepping are recorded.
- [ ] PTE 39-bit and PDE 40-bit address rules are confirmed on the target.
- [ ] PD alignment (4 KiB vs 64 KiB) is resolved.
- [x] GGTT allocation and GTTADDR write path are designed and software-tested;
  real aperture ownership and MMIO remain open.
- [ ] PDE cache and context-inactive rules are understood.
- [ ] PTE/PDE invalidation sequence and completion are specified.
- [ ] `ECOCHK`/`GAC_ECO_BITS` policy is decided.
- [ ] Coherency and IOMMU behavior are specified.
- [x] Scratch-page-table retention replaces live PDE removal in the software
  model.
- [x] Separate PDE/PTE encoders and distinct 40-bit/39-bit limits are tested.
- [x] A GGTT-resident directory provider and GTTADDR write path are modeled.
- [x] Register save/restore and activation rollback are tested against the
  software simulator; real MMIO recovery remains open.
- [ ] The experiment is approved for a disposable system only.
- [ ] No step requires i915 to remain bound during the physical run.

## 13. Status statement

Stage 3 (hardware PPGTT) remains **not implemented**. The CPU-side model now has
separate PDE/PTE encoders, correct 40-bit/39-bit limits, a GGTT-resident
directory provider with GTTADDR PDE writes, active/quiesced PDE-publication
rules, and scratch-table retention with deferred reclamation. Tests cover
hand-derived golden vectors, boundaries, directory residency, live-PDE safety,
and rollback. QEMU EDU page-table storage evidence was regenerated with the
corrected PTE encoding.

None of that is hardware page-table publication. Stage 3A now supplies a
checked GGTT allocator, a GTTADDR/flush/invalidate/activation abstraction with
saved-register rollback, a GGTT-backed directory provider, and an
independent-device ownership/teardown/recovery lifecycle, all tested with a
simulated backend. There is still no real PCI ownership, BAR mapping, MMIO
GTTADDR path, physical GGTT aperture ownership, register programming, TLB
invalidation, VM activation, or physical GPU page-walk evidence. i915 remains
the physical GPU owner. See `HASWELL_GEN75_STAGE3A_READINESS.md`.
