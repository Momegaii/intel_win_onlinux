# IDW architecture reference

This file preserves the project's lettered backbone. It is a reference map, not a tutorial or a claim that every subsystem is implemented.

## A — Privilege Rings & Modes (Ring 0 vs Ring 3)
- x86-64 CPU isolation, syscalls, interrupts, exceptions (#PF, #GP).

## B — Virtual Memory & Page Tables
- x86-64 page tables (4/5-level), CR3, PML4, PDPT, PD, PT, TLB, permissions, CPU MMU vs GPU page tables.

## C — Concurrency, Locking & Atomic Contexts
- Mutexes/semaphores vs spinlocks, memory barriers (`smp_mb`, `wmb`, `rmb`).

## D — Interrupts & Deferred Execution
- Linux top-half IRQ → bottom-half (softirq, tasklet, workqueue), Windows IRQL (PASSIVE_LEVEL, APC_LEVEL, DISPATCH_LEVEL, DIRQL) and DPCs; map IRQL to Linux context safely.

## E — Kernel Memory Allocators
- Linux page allocator, SLUB, `kmalloc` GFP_KERNEL vs GFP_ATOMIC, `vmalloc`; Windows `ExAllocatePool2` NonPagedPool/PagedPool.

## K — PCI & PCIe Enumeration
- Configuration space (Intel vendor `0x8086`, device ID, class `0x030000`), MSI/MSI-X, `pci_register_driver`.

## L — BARs (Base Address Registers) & MMIO
- PCIe register BARs; map with `ioremap` / `pci_iomap`, access with `ioread32` / `iowrite32`.

## M — DMA & Coherency
- DMA engine, commands/textures in RAM, cache coherency (CPU caches vs WC/UC), `dma_alloc_coherent`, scatter-gather lists (`sg_table`).

## N — IOMMU & GPU Page Tables (GTT / PPGTT)
- Intel VT-d IOMMU, GPU's GTT/PPGTT, GPU virtual addresses; distinguish IOMMU from Intel GTT/PPGTT. Haswell is Gen7.5/pre-Gen8; do not assume newer Intel PPGTT capabilities.

## O — Ring Buffers, Command Submission & Fences
- CPU writes GPU instructions into ring, updates TAIL register via MMIO, GPU executes commands until completion, interrupt and fence. This is the user's simplified model; validate the actual Haswell sequence before implementation.

## P — Linux DRM (Direct Rendering Manager) & GEM/TTM
- Linux GPU memory objects (`drm_gem_object`), cross-process sharing (`dma-buf`), command submission (`execbuf` / `vm_bind`).

## Q — Linux KMS (Kernel Mode Setting)
- Display pipeline `drm_framebuffer → drm_plane → drm_crtc → drm_encoder → drm_connector` (HDMI/eDP/DisplayPort), atomic commits and VBlank interrupts.

## R — Windows WDDM (`dxgkrnl.sys` + KMD)
- Microsoft-owned DirectX Graphics Kernel (`dxgkrnl.sys`) handles scheduling (VidSch) and video memory (VidMm), calls vendor KMD (`igdkmd64.sys`) via `DXGKRNL_INTERFACE` callbacks.

## S — User-Space Drivers (Mesa vs. Windows UMD)
- Shader compilation (SPIR-V/DXIL → Intel EU assembly), hardware command buffers in user space (Mesa ANV/Iris vs. Intel Windows UMD `igd10iumd64.dll` / `igvk64.dll`).

## T — Microsoft's WSL2 dxgkrnl on Linux
- Study open-source Linux `dxgkrnl` (`/dev/dxg`), exposes Windows D3DKMT* ioctls on Linux. Learn boundary between Windows UMD and kernel GPU management.

## U — Minimal ntoskrnl Shim Layer
- Implement core NT kernel primitives (`ExAllocatePool`, `KeInitializeSpinLock`, `KeAcquireSpinLockAtDpcLevel`, `RtlInitUnicodeString`, registry stubs) backed by Linux kernel APIs.

## V — Minimal dxgkrnl Callback Dispatcher
- Implement `DxgkInitialize` so a test miniport driver can register its `DxgkDdiAddDevice`, `DxgkDdiStartDevice`, and `DxgkDdiCreateAllocation` callbacks.

## W — PCIe & BAR Virtualization / Pass-Through
- Route Windows KMD hardware resource queries (`DxgkCbMapMemory`, `DxgkCbReadDeviceSpace`) to Linux PCI/DRM subsystem, starting against virtual QEMU GPU device before real hardware.

## X — Command Submission & Fence Translation
- Bridge submitted command buffer and fence signal from Windows KMD interface to Linux DRM ring/queue and signal completion back to user space.

## Y — Kernel Debugging, Crash Analysis & Fuzzing
- Master kgdb, kdump/crash, KASAN, lockdep so every kernel oops becomes a quick diagnosable fix.

## Z — Benchmarking & Profiling Real Workloads
- Measure latency, CPU overhead, frame pacing against native Linux (i915/xe + Mesa) using `intel_gpu_top` and RenderDoc.
