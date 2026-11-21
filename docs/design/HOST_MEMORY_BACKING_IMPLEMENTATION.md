# Host backing allocation implementation report

Status: implemented and tested. No physical GPU operation, MMIO access, BAR
mapping, i915 unbind, or PCI ownership change was performed.

## 1. Scope

This report traces the actual `MemoryManager` allocation callback and records
the first real backing provider in the native path. It does not claim a
physical GPU backend.

The implemented provider is `HostMemoryBackend`. It allocates real host pages
through a Linux `memfd`, maps them, and zero-faults every page. It implements
the existing `MemoryBackendOps` callback table and is exercised through the
existing `MemoryManager`.

It is deliberately not called a GPU backend:

- It does not create a GEM object.
- It does not obtain a DMA address.
- It does not create GPU-visible storage.
- It does not populate a Haswell page table.
- It does not publish mappings or invalidate hardware translations.

## 2. Exact callback and call chain

### 2.1 `MemoryManager` entry point

The allocation callback is `MemoryBackendOps::create_backing`.

Caller:

- `MemoryManager::create()` in `src/memory_manager.cpp`, around lines 102-125.
- It aligns the request to 4 KiB.
- It calls `ops_.create_backing(userdata_, aligned, &backing)` around line 118.
- It records only the returned opaque `backing` token in its `Resource`.

The callback is the ownership boundary: the backend must create or obtain
backing, and `MemoryManager` must not infer backing from the token.

### 2.2 i915-assisted path

```text
MemoryManager::create
  -> MemoryBackendOps::create_backing
  -> I915AssistedMemoryBackend::create_backing   (src/i915_memory_backend.cpp:53)
  -> I915GemResourceBackend::create              (src/i915_gem_resource_backend.cpp:59)
  -> DRM_IOCTL_I915_GEM_CREATE                   (src/i915_gem_resource_backend.cpp:70)
```

Result: a real i915 GEM object. i915 owns the backing, placement, DMA mapping,
PPGTT, and execution. IDW stores only the GEM handle and size metadata. This is
real kernel-managed memory, but it is not IDW-owned GPU memory.

### 2.3 New host-RAM path

```text
MemoryManager::create
  -> MemoryBackendOps::create_backing
  -> HostMemoryBackend::create_backing           (src/host_memory_backend.cpp:69)
  -> memfd_create                                (src/host_memory_backend.cpp:27, 88)
  -> ftruncate                                   (src/host_memory_backend.cpp:91)
  -> mmap                                        (src/host_memory_backend.cpp:96)
  -> memset (fault every page with zeroes)       (src/host_memory_backend.cpp:105)
  -> Allocation record with id/fd/address/bytes
```

Result: real host pages owned by IDW. The `memset` touches every page, so the
allocation is not merely a sparse file or a bookkeeping token. `mincore()`
confirms residency in the test.

The same provider implements:

- `destroy_backing`: refuses while bound; otherwise `munmap` and `close`.
- `bind`: records the `MemoryManager`-reserved GPU VA; no hardware write.
- `unbind`: clears the software binding record.
- `invalidate`: validates the VA/range; no hardware TLB operation.
- `submit`: performs `Noop` or `WriteDword` synchronously on the host mapping.
- `wait` / `quiesce`: fences are already complete.
- `cpu_read_dword` / `cpu_write_dword`: direct host-mapping access.

### 2.4 Kernel `/dev/idw` system-backed path

```text
IDW_IOCTL_ALLOCATION_CREATE
  -> kernel/idw_core_main.c:251
  -> vmalloc_user(size)                          (kernel/idw_core_main.c:252)
  -> shared user mmap via remap_vmalloc_range
```

Result: real zeroed kernel system pages. This is not connected to
`MemoryManager`, not DMA-pinned, not GPU-visible, and not page-table storage.

Default `/dev/idw` allocation creation without
`IDW_ALLOCATION_CREATE_SYSTEM_BACKED` allocates only `struct idw_allocation`
metadata (`kzalloc`), with no backing pages.

### 2.5 Native DMA page-table path

```text
idw_dma_pt_adapter
  -> idw_pt_dma_adapter
  -> idw_dma_page_alloc                         (kernel/idw_dma_memory.c:88)
  -> dma_alloc_coherent                         (kernel/idw_dma_memory.c:111)
```

Result: real coherent DMA pages with a real DMA address below 2^40. This path
is storage-only: it allocates page-table pages, not resource backing, and it
does not publish page tables, invalidate translations, or activate a VM. It is
not connected to `MemoryManager`.

### 2.6 Metadata-only or test-only paths

- `tests/memory-manager-test.cpp:31` `create_backing()` returns a counter and
  stores contents in `std::vector<uint32_t>`. It is a deterministic test
  double, not backing.
- `kernel/idw_test_miniport.c` `test_alloc()` returns an incrementing ID and
  explicitly logs `(metadata only)`.
- `I915GemResourceBackend` stores handle/size/binding metadata; the GEM object
  itself is real and i915-owned.
- The `MemoryManager` core stores a `uint64_t backing` token. A nonzero token
  proves only that a backend returned a value; it does not prove backing.

## 3. Current behavior versus required behavior

| Requirement | i915-assisted | Host provider | Kernel system-backed | Native DMA page-table | Fake test backend |
|---|---|---|---|---|---|
| Actual backing pages or GEM object | Real GEM object | Real host pages | Real `vmalloc_user` pages | Real coherent DMA pages | No, vector metadata |
| Real DMA address | i915-owned | No | No | Yes, 40-bit DMA address | No |
| GPU-visible backing | Yes, through i915 | No | No | Page-table storage only | No |
| Connected to GPU VA allocator | i915 execbuffer offsets | `MemoryManager` reservation record only | No | No | No |
| Populates actual page-table memory | i915-owned PPGTT | No | No | Yes, page-table pages | No |
| Publishes through physical hardware | i915-owned | No | No | No | No |

The new provider satisfies the first requirement and the `MemoryManager`
integration requirement. It intentionally does not satisfy the DMA, GPU-visible,
page-table-population, or hardware-publish requirements.

## 4. Missing backend interfaces

The native path still needs:

1. A device-scoped resource backing allocator that returns real DMA addresses
   for allocation sizes, not only 4 KiB page-table pages.
2. An IOMMU/domain policy and coherency contract for those backing pages.
3. A page-table population path that takes real resource DMA pages and writes
   real Haswell PTEs through the existing page-table adapter.
4. A hardware publication backend for GTTADDR, GGTT flush, invalidation, and
   per-engine activation.
5. Exclusive device ownership, BAR mapping, forcewake, engine quiescence, and
   reset/recovery.
6. Cache-policy and `PP_DIR_BASE` load-completion handling before any physical
   PPGTT activation.

These are missing; the host provider does not stand in for them.

## 5. Existing reusable components

- `MemoryManager` (`include/idw/memory_manager.hpp`,
  `src/memory_manager.cpp`): allocation, VA reservation, bind/unbind,
  invalidation recovery, submission references, and teardown guards.
- `idw_gpuva_space` (`src/gpuva_model.c`): CPU-side GPU VA interval allocator.
- `idw_hsw_ppgtt_model` (`src/haswell_ppgtt_model.c`): corrected Haswell
  PDE/PTE model and translation checks.
- `idw_pt_dma_adapter` (`src/pt_dma_adapter.c`): exact 4 KiB page-table storage
  validation, scope/cookie checks, and device synchronization.
- `idw_dma_provider` (`kernel/idw_dma_memory.c`): real `dma_alloc_coherent`
  pages for an IDW-owned PCI device, with ownership and 40-bit checks.
- `idw_dma_pt_adapter` (`kernel/idw_dma_pt_adapter.c`): kernel binding from the
  DMA provider to the page-table adapter.
- `I915GemResourceBackend` (`src/i915_gem_resource_backend.cpp`): real GEM
  object create/close through i915.
- GGTT allocator, GGTT directory provider, Haswell hardware abstraction, and
  device lifecycle: software/simulated infrastructure for the future physical
  backend.

## 6. Smallest implementation path to real backing

Implemented now:

1. Add `HostMemoryBackend` as a real `MemoryBackendOps` provider.
2. Allocate a real `memfd`, size it, map it, and zero-fault every page.
3. Integrate it with `MemoryManager` without changing the manager or adding a
   new abstraction layer.
4. Prove real page residency and contents with `mincore()` and cross-page
   writes.

Next smallest steps, in order:

1. Extend the existing `idw_dma_provider` from 4 KiB page-table pages to
   resource-sized coherent allocations, or add a resource-sized allocation
   function beside it, using the same ownership and 40-bit checks.
2. Expose that provider through the existing `MemoryBackendOps` table as a
   native DMA backend. Do not add another manager layer.
3. Feed the resulting real DMA pages into the existing page-table adapter and
   PPGTT model. The page-table pages are already real DMA pages; the missing
   piece is resource-page mapping, not a new table model.
4. Only after exclusive ownership and all Stage 3B gates, connect the existing
   hardware abstraction to a real MMIO/GTTADDR backend.

## 7. Tests proving actual backing allocation

Target:

```sh
make host-memory-backend-test
```

The test is in `tests/host-memory-backend-test.cpp`.

It proves:

- `memfd` allocation returns a nonzero backing ID.
- The mapping is page-aligned and the requested byte count is real.
- `mincore()` reports every page resident after the provider's zero-fault.
- A distinct pattern written to each page reads back exactly.
- Two allocations have distinct addresses and independent contents.
- `MemoryManager::create()` causes a real allocation, not a fake token.
- `MemoryManager` reserves a GPU VA, binds it, submits a write, waits, reads
  back the value, unbinds, and destroys the resource.
- Destroy while bound fails; cleanup releases the allocation.
- Invalid alignment, out-of-range dword access, double bind, wrong-VA unbind,
  and invalid fences are rejected.
- `close_all()` refuses while bound and releases after unbind.

Sanitizer run:

```sh
cc ... -fsanitize=address,undefined ...
g++ ... -fsanitize=address,undefined ...
ASAN_OPTIONS=detect_leaks=1 ./build/sanitize/host-memory-backend-test
```

The test passed under ASan/UBSan with leak detection.

## 8. What is not proven

- No physical GPU mapping.
- No Haswell PPGTT page-table publication.
- No GPU TLB invalidation or engine activation.
- No native DMA resource mapping.
- No IOMMU policy or coherency proof.
- No independent GPU execution.
- No i915 detach/rebind or exclusive ownership.

The host provider is a real backing-allocation step, not a physical GPU
backend. CPU tests, simulation, QEMU, and i915-assisted execution remain
separate evidence levels.
