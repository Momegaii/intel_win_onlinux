# Stage 3C D2 ownership-aware probe-failure evidence (2026-11-21)

Baseline: `master` @ `730cba04979e1185bbf01b6a464c16cbbae930c6`.
No physical GPU, i915 unbind, MMIO, PCI ownership change, or Stage 3D work.

## Commands and results

| Command | Exit | Result | Log | SHA-256 |
|---|---:|---|---|---|
| `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build CC=cc` | 0 | PASS | `kernel-build.log` | `d2186c42eeb693d15018b49701f84b6422d1aa4a6da984ecd4fe4200dce627e2` |
| `make contract-test backend-contract-test i915-resource-test i915-memory-backend-test gpu-submission-test memory-manager-test host-memory-backend-test native-dma-backend-test native-dma-fault-test native-dma-backend-test-static native-dma-fault-test-static abi-test ppgtt-model-test pt-dma-adapter-test ggtt-allocator-test haswell-gtt-hw-test device-lifecycle-test cli-test` | 0 | PASS for executed tests; native DMA tests SKIP without `/dev/idw-edu-dma` | `software-tests.log` | `755f6bb4c51d192630d27e8493d3411c59bc09171bc0a4757ad13fae266a3fdb` |
| `make ppgtt-model-test` | 0 | PASS | `ppgtt-model-test.log` | `3d3b82b34484d9e5906e6496c2d573673fb20f71adb657402e22802961429218` |
| `make native-dma-fault-test` | 0 | SKIP outside QEMU (not a pass) | `native-dma-fault-test.log` | `827412a31983606de1c2d900071515e04aec7abff507384619bcce7e467f25cd` |
| `make stage3a-test` | 0 | PASS | `stage3a-test.log` | `0b0a960ce33bb5499177a32fb0892f8eee04b2b6deb602230ae4c57c05770a1b` |
| `make stage3a-sanitize-test` | 0 | PASS | `stage3a-sanitize.log` | `73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294` |
| `KDIR=/lib/modules/6.12.117+deb13-amd64/build IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 make qemu-dma-test` | 0 | PASS; QEMU status 33 | `qemu-dma-normal.log`; raw `qemu-dma-normal-run.log` | `7f2c25657a5f3089e5c1015a2451db795fcda64cead58913c34b92dd53e9aff8`; raw `4f3248bcff21901575f11466a265449283a2f0505d9a10dd0a0d754b5921c15b` |
| `KDIR=/lib/modules/6.12.117+deb13-amd64/build IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 ./tests/qemu-dma/run.sh --probe-failure` | 0 | PASS; QEMU status 33; two probe-failure/recovery rounds | `qemu-dma-probe-failure.log`; raw `qemu-dma-probe-failure-run.log` | `cd36b37d97fcea714fa4de901e19e40401b36ab771582943fbe6b86abab5a634`; raw `638bdd21a249717c82e407316ac03e10ddf888a8288442910702ceb5d26b3dfa` |
| `make test` | **2** | FAIL only at `tests/smoke.sh`: no Intel render node | `make-test.log` | `27ebc1362554465bbece3e88d8e4b77f79cab78425b6d9c21312c33ca2632e76` |

## Actual probe-failure evidence

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
```

The retained owner was reachable through `owner_count=1` while quarantined.
PCI state remained enabled until the injected dependency was released. The
second round repeated the failure and recovery without leaving a retained
owner.

## Normal QEMU evidence

```text
IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release
IDW_QEMU_DMA: provider finalization refusal retained owner and recovered after release
IDW_QEMU_DMA: owner 1 quarantined adapter refs state=QUARANTINED count=1
IDW_QEMU_DMA: owner 1 provider cleanup blocked: -16 pci_enabled=0 regions=0 master=0 bar=0
IDW_QEMU_DMA: owner 1 adapter recovered, provider still blocked state=QUARANTINED count=1
IDW_QEMU_DMA: owner 1 released state=RELEASED
IDW_QEMU_DMA: owner 1 released after all dependencies drained
IDW_QEMU_DMA: remove released all provider resources result=0
QEMU exit status: 33
```

## Limitations

- QEMU EDU is virtual-device evidence, not Haswell hardware evidence.
- Stage 3C closure is not claimed; Check 15 physical evidence is unavailable.
- The driver publishes one active EDU owner at a time. The registry is bounded
  at 8 retained owners; repeated probe failures are tested, concurrent EDU
  devices are not.
- `make test` still stops at the pre-existing no-render-node smoke check.
- No push, merge, or pull request is included.
