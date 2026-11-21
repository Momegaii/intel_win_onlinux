# Stage 3C physical Haswell evidence attempt (2026-11-21)

**Result:** `TARGET_GPU_NOT_PRESENT`

This run was executed inside a Google Compute Engine VM. The target
`0000:00:02.0` is absent, `/dev/dri` is absent, `i915` is not loaded, and the
container has no IOMMU groups or `/dev/vfio`.

This is evidence of absence only. It does **not** close Stage 3C and does
**not** demonstrate physical Haswell operation.

| File | SHA-256 |
|---|---|
| `environment.log` | `70da7ed8b78cc2c697c67ea9a057868640f9170cb16f1120633253ea9227330e` |
| `lspci.log` | `9941d0c46506a061761a25d2db92be61f53becefffa03eb1bcfdeb22a90f5a19` |
| `pci.log` | `4f07afa30dacd16e3e33d42276376465172584dc8712fde361adc65681f32d7f` |
| `target.log` | `ff8d221c43c6e5fefa7e2e047215603fe7e9c427bf2f56c1825dd98df318f646` |
| `drm.log` | `72c4d4d8301b56917f20a1ddf030aad0e12c4654be7c00c6b360181b5555b95b` |
| `modules.log` | `3a8cd996b9c7cf053cfec55d294161c8949c3d37583b311c039e6f4dd96c6ce3` |
| `summary.txt` | `3cc0691f9f7fb97c56e823e1293807edf213e3e10cd8ab22936d53e3e4251de6` |

The collector is `tests/physical/haswell-evidence.sh`. The runbook is
`docs/design/STAGE3C_PHYSICAL_EVIDENCE_RUNBOOK.md`.
