# Stage 3C physical Haswell evidence runbook

**Status:** Check 15 physical evidence collected 2026-11-21; Stage 3C remains open.

Check 15 requires a read-only check on a host that exposes the Haswell GPU:

```sh
lspci -nnk -s 00:02.0
cat /sys/bus/pci/devices/0000:00:02.0/driver
```

The target is Intel Haswell `8086:0412` with `i915` bound. A QEMU EDU device,
a software DRM device, or a QEMU display adapter is not a substitute.

## Requirements

- Bare-metal Haswell host, or a VM with the physical GPU passed through.
- The target GPU visible at PCI address `0000:00:02.0`.
- `i915` loaded and bound to that device.
- `/dev/dri/renderD128` present.
- No unbind, MMIO, BAR write, reset, or driver replacement is performed.

## Run

```sh
make physical-evidence PHYSICAL_EVIDENCE_DIR=tests/physical/evidence-$(date -u +%Y%m%dT%H%M%SZ)
```

For a read-only HTTP wrapper that can be reached through a private tunnel, see
`docs/design/STAGE3C_EVIDENCE_API.md`.

Use a new directory for each run. To overwrite an existing evidence
directory deliberately, pass `--force` to the script.

The collector writes:

| File | Contents |
|---|---|
| `environment.log` | UTC time, kernel, virtualization, CPU, kernel command line |
| `lspci.log` | Full `lspci -nnk` output when `lspci` is installed |
| `pci.log` | Sysfs vendor, device, class, and driver for every PCI device |
| `target.log` | Target `0000:00:02.0` presence, IDs, driver, and `lspci -nnk -s 00:02.0` |
| `drm.log` | `/dev/dri` and `/sys/class/drm` |
| `modules.log` | `i915`, render node, IOMMU groups, and `/dev/vfio` |
| `summary.txt` | Machine-readable result |

## Results

| Result | Exit | Meaning |
|---|---:|---|
| `PASS` | 0 | `8086:0412` is present, bound to `i915`, with a render node |
| `TARGET_GPU_NOT_PRESENT` | 2 | No `0000:00:02.0`; physical evidence cannot be collected |
| `TARGET_GPU_MISMATCH` | 3 | Device exists but is not `8086:0412` |
| `TARGET_GPU_NOT_BOUND_TO_I915` | 4 | Target exists but `i915` is not the driver |
| `RENDER_NODE_ABSENT` | 5 | i915 is bound but no `/dev/dri/renderD128` |

## Current environment result

### 2026-11-21 bare-metal host — PASS

The Check 15 run on the target host reported:

- OS: Arch Linux
- Kernel: `7.3.5-arch1-1`
- CPU: Intel Core i5-4460 @ 3.20 GHz
- Chipset: Intel B85
- `systemd-detect-virt`: `none`
- Target: `0000:00:02.0`
- Vendor/device: `0x8086:0x0412`
- Driver: `i915`
- Render node: `/dev/dri/renderD128`
- IOMMU groups: `0`

Result: `PASS`. Evidence:
`tests/physical/evidence-2026-11-21-host/`.

### 2026-11-21 Google Compute Engine VM — TARGET_GPU_NOT_PRESENT

The earlier attempt ran in a Google Compute Engine VM:

- CPU: AMD EPYC 9B14
- `systemd-detect-virt`: `container-other`
- PCI devices: QEMU host bridge, PIIX4 bridges, gVNIC, virtio-rng, NVMe
- `0000:00:02.0`: absent
- `/dev/dri`: absent
- `i915`: not loaded
- IOMMU groups: 0
- `/dev/vfio`: absent

Result: `TARGET_GPU_NOT_PRESENT`. Loading `i915` also failed with
`Operation not permitted`; the container has no `CAP_SYS_MODULE`. This is
evidence of absence, not physical Haswell evidence.

## Closure rule

Check 15 is satisfied only by a `PASS` summary and raw logs from target
hardware. Do not close it from QEMU, emulated display adapters, synthetic PCI
devices, or a host that does not expose the GPU. The 2026-11-21 host run
satisfies Check 15; it does not close Stage 3C, because the remaining
lifecycle, failure-path, and physical execution checks still apply.
