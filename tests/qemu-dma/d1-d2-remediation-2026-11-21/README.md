# Stage 3C D1/D2 targeted remediation evidence (2026-11-21)

Baseline: `master` @ `2514e43156ebc9ea3ce8152dc8fe51d0b4144a2a`.
No physical GPU, i915 unbind, MMIO, PCI ownership change, or Stage 3D work.

## Commands and results

| Command | Exit | Result | Log | SHA-256 |
|---|---:|---|---|---|
| `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build CC=cc` | 0 | PASS | `kernel-build.log` | `d2186c42eeb693d15018b49701f84b6422d1aa4a6da984ecd4fe4200dce627e2` |
| `make contract-test backend-contract-test i915-resource-test i915-memory-backend-test gpu-submission-test memory-manager-test host-memory-backend-test native-dma-backend-test native-dma-fault-test native-dma-backend-test-static native-dma-fault-test-static abi-test ppgtt-model-test pt-dma-adapter-test ggtt-allocator-test haswell-gtt-hw-test device-lifecycle-test cli-test` | 0 | PASS for executed tests; native DMA tests SKIP without `/dev/idw-edu-dma` | `software-tests.log` | `755f6bb4c51d192630d27e8493d3411c59bc09171bc0a4757ad13fae266a3fdb` |
| `make ppgtt-model-test` | 0 | PASS | `ppgtt-model-test.log` | `3d3b82b34484d9e5906e6496c2d573673fb20f71adb657402e22802961429218` |
| `make native-dma-fault-test` | 0 | SKIP outside QEMU (not a pass) | `native-dma-fault-test.log` | `827412a31983606de1c2d900071515e04aec7abff507384619bcce7e467f25cd` |
| `make stage3a-sanitize-test` | 0 | PASS | `stage3a-sanitize.log` | `73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294` |
| `KDIR=/lib/modules/6.12.117+deb13-amd64/build IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 make qemu-dma-test` | 0 | PASS; QEMU status 33 | `qemu-dma-test.log`; raw `qemu-dma-run.log` | `cf31a587d4b9e5b0cf63e1a60036e7b04d6baf4a3769bf6ea43b356e105adc90`; raw `d1523bc0e537adeb8cf3decdaccb2d6fdc5ee1a5e2381106367892e62185a439` |
| `make test` | **2** | FAIL only at `tests/smoke.sh`: no Intel render node | `make-test.log` | `27ebc1362554465bbece3e88d8e4b77f79cab78425b6d9c21312c33ca2632e76` |

## D1 counter snapshots (QEMU serial)

```text
COUNTERS D1_BEFORE alloc=2 mapped=2 ptes=1 pt=1 va=1 q=0
COUNTERS D1_FAILURE alloc=2 mapped=2 ptes=2 pt=1 va=2 q=1
COUNTERS D1_AFTER_RECOVER alloc=2 mapped=2 ptes=1 pt=1 va=1 q=0
COUNTERS D1_RECOVERY alloc=0 mapped=0 ptes=0 pt=0 va=0 q=0
```

Order: `allocated_pages / mapped_pages / populated_ptes / pt_pages / va_reservations / quarantined_pages`.

## D2 provider finalization

QEMU serial confirms both the retained-owner path and recovery:

```text
IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release
IDW_QEMU_DMA: provider finalization refusal retained owner and recovered after release
```

The probe failure path now retains `struct idw_edu_test` when provider/adapter
finalization refuses cleanup; it does not `kfree()` memory still referenced by
the provider.
