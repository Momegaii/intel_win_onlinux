# Stage 3C physical Haswell evidence (2026-11-21 host run)

**Result:** `PASS`

**Scope:** Stage 3C Check 15 read-only binding evidence only. This run did not
unbind i915, map a BAR, perform MMIO, submit GPU work, activate PPGTT, or
change PCI ownership. It does not by itself close Stage 3C.

## Host

- OS: Arch Linux
- Kernel: `7.3.5-arch1-1`
- CPU: Intel Core i5-4460 @ 3.20 GHz
- Chipset: Intel B85
- Virtualization: `none` (bare metal)
- Target: `0000:00:02.0`
- Vendor/device: `0x8086:0x0412`
- Driver: `i915`
- Render node: `/dev/dri/renderD128`
- IOMMU groups: `0`
- `/dev/vfio/vfio`: present, no IOMMU group attached

## Collection

The run was made with `tests/physical/haswell-evidence.sh` through the
read-only `tools/idw-evidence-api.py` wrapper.

- API run ID: `20261121T131640Z-38815eb8`
- Collector exit code: `0`
- Collector output: `PHYSICAL_EVIDENCE_RESULT=PASS`

The raw files were written by the collector. They were not edited after
collection.

## Evidence

`target.log` records:

```text
TARGET_GPU_PRESENT: 0000:00:02.0 vendor=0x8086 device=0x0412 driver=i915
00:02.0 VGA compatible controller [0300]: Intel Corporation Xeon E3-1200 v3/4th Gen Core Processor Integrated Graphics Controller [8086:0412] (rev 06)
        DeviceName:  Onboard IGD
        Subsystem: ASUSTeK Computer Inc. Device [1043:8534]
        Kernel driver in use: i915
        Kernel modules: i915
sysfs_vendor=0x8086
sysfs_device=0x0412
sysfs_driver=i915
```

`summary.txt`:

```text
target_present=true
target_vendor=0x8086
target_device=0x0412
target_driver=i915
result=PASS
```

## Files

| File | Contents |
|---|---|
| `environment.log` | UTC time, kernel, virtualization, CPU, kernel command line |
| `lspci.log` | Full `lspci -nnk` output |
| `pci.log` | Sysfs vendor/device/class/driver for each PCI device |
| `target.log` | Target presence, IDs, driver, and `lspci -nnk -s 00:02.0` |
| `drm.log` | `/dev/dri` and `/sys/class/drm` |
| `modules.log` | i915 module, render node, IOMMU groups, `/dev/vfio` |
| `summary.txt` | Machine-readable result |

## Boundary

This is physical i915 binding evidence. It is not evidence of an IDW-owned
GGTT, physical PPGTT activation, MMIO access, GPU submission, or independent
Haswell execution. i915 remained the hardware owner throughout.
