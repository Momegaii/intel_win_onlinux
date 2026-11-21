# Native DMA resource backing implementation

Status: implemented and validated inside the QEMU EDU guest. No physical
Haswell operation, MMIO access, BAR mapping, i915 unbind, or PCI ownership
change was performed. i915 remains bound on the host.

## 1. What changed

The previous host provider gave `MemoryManager` real CPU pages, but those pages
had no DMA address. The new path adds the third allocation reality:

| Backend | Backing owner | DMA address | GPU visibility | Hardware page walk |
|---|---|---|---|---|
| `HostMemoryBackend` | IDW (`memfd`) | none | none | none |
| `I915AssistedMemoryBackend` | i915 GEM | i915-owned | through i915 PPGTT | i915 |
| `NativeDmaMemoryBackend` | IDW (test module) | real Linux DMA address | through the IDW CPU PPGTT model | simulated only |

`NativeDmaMemoryBackend` implements the existing `MemoryBackendOps` table. It
allocates device-scoped resource pages and page-table pages from the QEMU EDU
test module, maps them into user space, populates the existing Haswell Gen7.5
PPGTT model with the returned DMA addresses, and performs the manager's
synchronous dword submission on the mapped resource.

## 2. Kernel test interface

The interface is test-only and lives in `include/uapi/idw_dma_test.h`. It is
exposed by `kernel/idw_edu_dma_test_main.c` as `/dev/idw-edu-dma` while the QEMU
EDU PCI driver is bound.

Resource ioctls:

- `IDW_DMA_TEST_IOCTL_ALLOC`: allocate `bytes` of DMA-mapped resource backing.
  Returns a resource cookie, device cookie, generation, and page count.
- `IDW_DMA_TEST_IOCTL_FREE`: release an unmapped resource. The provider refuses
  while a VMA still references it.
- `IDW_DMA_TEST_IOCTL_SEGMENTS`: return the real DMA address, length, device
  scope, and mapping cookie for each page in a range.
- `IDW_DMA_TEST_IOCTL_SYNC`: run the DMA sync direction on a resource range.
- `IDW_DMA_TEST_IOCTL_STATS`: return provider resource/page/VMA/quarantine
  counters.
- `IDW_DMA_TEST_IOCTL_FAULT`: set test-only backing allocation or DMA mapping
  failure indices.

Page-table ioctls:

- `IDW_DMA_TEST_IOCTL_ALLOC_PT` / `FREE_PT`: allocate or free one coherent
  4 KiB page-table page.
- `IDW_DMA_TEST_IOCTL_SYNC_PT`: make page-table writes visible to the device.

Mmap offsets:

- resource cookie `N`: byte offset `N * 4096`, length `page_count * 4096`.
- page-table cookie `N`: byte offset `(2^32 + N) * 4096`, length 4096.

The kernel module validates the cookie, length, alignment, and device scope
before mapping. Resource mappings use `vm_insert_page()` over the DMA-mapped
pages; page-table mappings use `dma_mmap_coherent()`.

## 3. User-space backend

`include/idw/native_dma_backend.hpp` / `src/native_dma_backend.cpp`:

```text
MemoryManager::create
  -> NativeDmaMemoryBackend::create_backing
  -> IDW_DMA_TEST_IOCTL_ALLOC
  -> mmap(resource cookie)
  -> Allocation record

MemoryManager::bind
  -> IDW_DMA_TEST_IOCTL_SEGMENTS
  -> idw_hsw_ppgtt_model_create (first bind)
  -> idw_hsw_ppgtt_map
  -> page-table allocations through IDW_DMA_TEST_IOCTL_ALLOC_PT
  -> PTE publication and IDW_DMA_TEST_IOCTL_SYNC_PT
```

The backend's `translate()` calls `idw_hsw_ppgtt_translate()` and returns the
39-bit DMA address encoded in the PTE. The QEMU test compares that value with
the real DMA address reported by the kernel for the same page.

Lifetime rules:

- `destroy_backing` refuses while bound and while a resource VMA is mapped.
- `unbind` clears PTEs, completes the model invalidation, and only then marks
  the allocation unbound.
- `close_all` refuses while allocations remain, proves PPGTT inactivity, then
  destroys the model with hardware quiescence **not** asserted and frees page
  tables through the model's callbacks.
- The kernel provider refuses `FREE` while `resource->mappings` is nonzero and
  refuses removal while any page, resource, or VMA reference remains.
- A VMA holds a file reference, so `release` cannot run while the mapping is
  live. VMA `open`/`close` also hold the module reference; removal therefore
  waits or fails closed instead of invalidating a live mapping.

`make native-dma-backend-test` runs the lifecycle through `MemoryManager`.
`make native-dma-fault-test` injects create, page-table, mmap, sync, bind,
unbind, and destroy failures, then checks retry, quarantine, counter drain, and
live-VMA rejection. Both skip on a host without `/dev/idw-edu-dma`; the QEMU
guest invokes the static builds with `--require-device`, so a missing device is
a failure there.

## 4. QEMU evidence

Command used in the Debian 13 container:

```sh
KDIR=/lib/modules/6.12.117+deb13-amd64/build \
IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 \
make qemu-dma-test
```

Result: `QEMU exit status: 33` (isa-debug-exit success). The guest log contains:

```text
IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified
IDW_QEMU_DMA: backing allocation/mapping fault injection verified
IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release
PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup
PASS: native DMA backend fault injection, teardown retry, VMA lifetime, and ownership counters
IDW_QEMU_DMA: remove released all provider resources result=0
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
```

Full excerpts and log hashes: `tests/qemu-dma/STAGE3C_REMEDIATION_EVIDENCE.md`.

## 5. Boundaries

This is real Linux DMA memory for an IDW-bound virtual PCI device, not Haswell
GPU memory. The following remain missing or simulated:

- The page directory is an anonymous user-space page. It is not a GGTT
  aperture page and no hardware GTTADDR path consumes it.
- The PPGTT page walk is the CPU model. No Intel GPU consumes the PTEs.
- There is no MMIO, forcewake, engine activation, reset, or physical Haswell
  DMA.
- The kernel interface is a QEMU test UAPI, not a production IDW ABI.
- A single bind supports at most 64 pages because the test UAPI returns at most
  64 segments. Larger resources need a production scatter/gather contract.
- DMA addresses must fit the 39-bit mapped-page limit enforced by the Haswell
  PTE encoder; the kernel provider rejects addresses outside that range.

The next physical step remains exclusive device ownership, a real GGTT
directory, cache-policy programming, `PP_DIR_BASE` load-completion ordering,
and a hardware page walk. None of those is claimed here.
