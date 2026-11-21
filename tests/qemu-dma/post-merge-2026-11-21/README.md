# Stage 3C post-merge evidence index (2026-11-21)

Merged baseline: `master` @ `2514e43156ebc9ea3ce8152dc8fe51d0b4144a2a`.
Remediation commit: `5f3d4ceb3ba023110634ac0a3af1c3e6adde1231` (ancestor of baseline).
No implementation or test source was changed.

| Claim | Raw evidence | SHA-256 |
|---|---|---|
| Host has no target Haswell GPU, no `/dev/dri`, no i915; no QEMU EDU on host | `hardware-detection.log` | `e4120ab5bc518958375030c5567c1a166df3a7c317109ec62c80a4f67b9311c6` |
| Merged kernel modules build | `kernel-build.log` | `7049243fd20b254a4582874269b7358c0198ab75751205bdc4735788f0d44009` |
| Software suite passes; native DMA tests skip without `/dev/idw-edu-dma` | `software-tests.log` | `755f6bb4c51d192630d27e8493d3411c59bc09171bc0a4757ad13fae266a3fdb` |
| PPGTT model tests pass | `ppgtt-model-test.log` | `3d3b82b34484d9e5906e6496c2d573673fb20f71adb657402e22802961429218` |
| Native DMA fault test skips outside QEMU (not a pass) | `native-dma-fault-test.log` | `827412a31983606de1c2d900071515e04aec7abff507384619bcce7e467f25cd` |
| Stage 3A sanitizer suite passes | `stage3a-sanitize.log` | `73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294` |
| QEMU EDU TCG happy path and fault tests pass; QEMU status 33 | `qemu-dma-test-stdout.log`; raw serial `qemu-dma-run.log` | stdout `9949eea93150bef61e10486ac1cdda5219b65c83c4083992942c4f8410c5da04`; raw `bbb97c11e177cdc50ed38113b7832bd15634645047b4ccd7f4b7af3283d0ee7e` |
| Pre-existing QEMU raw log preserved before the new run | `qemu-dma-run-before.log` | `0895a63e3633841a7a482543b4d7a5074867cd443edb8ee48260f550c9e48d07` |
| Full `make test` fails only at `tests/smoke.sh` because no Intel render node exists | `make-test.log` | `27ebc1362554465bbece3e88d8e4b77f79cab78425b6d9c21312c33ca2632e76` |
| Confirmed D1: post-publication bind failure consumes the wrong VA reservation and blocks cleanup | `defect-repro-qemu.log` | `5233abbfc9b99b885f72eb894bd46f94c676330b5b02577a46c1001f8807c871` |

`qemu-dma-run.log` is the QEMU serial log from the standard harness. The
`defect-repro-qemu.log` is a diagnostic run using a temporary initramfs that
replaced only the guest fault-test binary; it did not modify repository source
or tests. It is evidence for defect D1, not a standard test pass.
