# Stage 3C D2 ownership-aware probe-failure final evidence (2026-11-21)

Baseline: `master` @ `5a46435ae9e6a5667a8eec3be26330a9ee1fd019`.
The D2 lifetime corrections in this evidence are committed on `master`.
No physical GPU, i915 unbind, MMIO, PCI ownership change, or Stage 3D work.

## Corrections exercised by this run

- `pci_dev_get()`/`pci_dev_put()` now pin the retained owner's PCI device.
- `pci_disable_device()` runs before `pci_release_regions()`.
- Remove-path adapter failure with provider success no longer leaves
  `provider_ready` set on an inactive provider.
- Registry capacity is reserved before probe cleanup, so a failed probe cannot
  lose its slot between cleanup refusal and registry insertion.
- Synthetic owner lifecycle registration happens after its dependencies exist.
- Probe-failure retention is explicitly marked as a QEMU EDU test-only
  quarantine path; production probe failure must release all resources.

## Commands and results

| Command | Exit | Result | Log | SHA-256 |
|---|---:|---|---|---|
| `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build CC=cc W=1` | 0 | PASS, no warnings | `kernel-build.log` | `7049243fd20b254a4582874269b7358c0198ab75751205bdc4735788f0d44009` |
| Software test group (`make contract-test ... cli-test`) | 0 | PASS for executed tests; native DMA tests SKIP without `/dev/idw-edu-dma` | `software-tests.log` | `755f6bb4c51d192630d27e8493d3411c59bc09171bc0a4757ad13fae266a3fdb` |
| `make ppgtt-model-test` | 0 | PASS | `ppgtt-model-test.log` | `3d3b82b34484d9e5906e6496c2d573673fb20f71adb657402e22802961429218` |
| `make native-dma-fault-test` | 0 | SKIP outside QEMU | `native-dma-fault-test.log` | `827412a31983606de1c2d900071515e04aec7abff507384619bcce7e467f25cd` |
| `make stage3a-test` | 0 | PASS | `stage3a-test.log` | `0b0a960ce33bb5499177a32fb0892f8eee04b2b6deb602230ae4c57c05770a1b` |
| `make stage3a-sanitize-test` | 0 | PASS | `stage3a-sanitize.log` | `73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294` |
| `make qemu-dma-test` | 0 | PASS; QEMU status 33 | `qemu-dma-normal.log`; raw `qemu-dma-normal-run.log` | `7d148c0f43300c489c6b27ab9fce83ee1e6eb2902b4382927e606adc1ee09d24`; raw `41bfa855b330ebb4ab3bb4f62fcab39165660e746d2e6e388110276231f4f276` |
| `tests/qemu-dma/run.sh --probe-failure` | 0 | PASS; QEMU status 33; two rounds | `qemu-dma-probe-failure.log`; raw `qemu-dma-probe-failure-run.log` | `2d1956fc53825ba2da1c1022a6bd45483f2f3187062cd16f2b1db449040409de`; raw `c4a6bfb9480e984e7e4bd36993d1e0c3201e4e4a8d707cc76d461ef00a46eb10` |
| `make test` | **2** | Stops only at `tests/smoke.sh`: no Intel render node | `make-test.log` | `27ebc1362554465bbece3e88d8e4b77f79cab78425b6d9c21312c33ca2632e76` |

## Final probe-failure evidence

```text
IDW_QEMU_DMA: owner 0 provider cleanup blocked: -16 pci_enabled=1 regions=1 master=1 bar=1
IDW_QEMU_DMA: probe failure retained owner 1 state=QUARANTINED count=1
IDW_QEMU_DMA: owner 1 released probe-failure test page
IDW_QEMU_DMA: owner 1 recovering from quarantine
IDW_QEMU_DMA: owner 1 PCI cleanup begin enabled=1 regions=1 master=1 bar=1
IDW_QEMU_DMA: owner 1 PCI cleanup complete enabled=0 regions=0 master=0 bar=0
IDW_QEMU_DMA: owner 1 released state=RELEASED
IDW_QEMU_DMA: probe failure round 1 owner recovered
IDW_QEMU_DMA: probe failure round 2 owner recovered
IDW_QEMU_DMA: PASS: probe failure cleanup, quarantine, retry, repeated owner release completed
QEMU exit status: 33
```

## Final normal-path cleanup evidence

```text
IDW_QEMU_DMA: owner 0 PCI cleanup begin enabled=1 regions=1 master=1 bar=1
IDW_QEMU_DMA: owner 0 PCI cleanup complete enabled=0 regions=0 master=0 bar=0
IDW_QEMU_DMA: remove released all provider resources result=0
IDW_QEMU_DMA: driver removal completed successfully
QEMU exit status: 33
```

## Limitations

- QEMU EDU is virtual-device evidence, not Haswell hardware evidence.
- Stage 3C closure is not claimed; Check 15 physical evidence is unavailable.
- Probe-failure quarantine is a test-only control path, not a production PCI
  probe pattern.
- The driver publishes one active EDU owner at a time; repeated failures are
  tested, concurrent EDU devices are not.
- `make test` still stops at the pre-existing no-render-node smoke check.
- No push, merge, or pull request is included.
