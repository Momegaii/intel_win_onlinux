# Stage 3C — Audit Evidence Ledger

> **Historical ledger.** Remediation evidence is in
> `tests/qemu-dma/STAGE3C_REMEDIATION_EVIDENCE.md`; the post-remediation
> status is in `docs/design/STAGE3C_REMEDIATION_REPORT.md`.

**Audit:** IDW Stage 3C 17-point PR audit  
**PR:** [#5](https://github.com/Momegaii/intel_win_onlinux/pull/5)  
**HEAD:** `fa2e91879b02526b4a31615ddf19561443d8e3cc`  
**Base:** `c596f5b8047287b10b8f5b22da9c0613746185bd`  
**Date:** 2026-11-21 UTC  
**Mode:** read-only; no physical GPU access; no commit, push, or merge

This ledger records commands actually executed during the audit, their exit codes, relevant raw output, and SHA-256 hashes of the actual log files. It does not treat a skipped test as a pass.

---

## 1. Clean-tree preflight

Command:

```sh
git status --porcelain=v1
```

Exit: `0`. Output: **empty**. The worktree was clean before any audit output was written.

Additional preflight:

```text
HEAD    fa2e91879b02526b4a31615ddf19561443d8e3cc
branch  master
shallow true
base    c596f5b8047287b10b8f5b22da9c0613746185bd
```

`git --no-pager log --oneline -4`:

```text
fa2e918 (HEAD -> master, origin/master, origin/HEAD) Add native DMA resource backing through MemoryManager
b9f3dd6 Add real host backing provider to MemoryManager
c596f5b Merge Stage 3B design closure and PPGTT hardening
70d60fb Stage 3B: close design audit and harden PPGTT activation
```

`git diff --stat c596f5b..fa2e918` reported 24 files changed, 3,690 insertions, 21 deletions. The complete changed-file list was enumerated; the host provider, native DMA backend, kernel DMA provider, UAPI, QEMU harness, PPGTT model, Makefile, and relevant documentation/evidence claims were inspected.

No audit file existed at preflight:

```text
ls: cannot access 'docs/design/STAGE3C_17_POINT_PR_AUDIT.md': No such file or directory
ls: cannot access 'tests/qemu-dma/STAGE3C_AUDIT_EVIDENCE.md': No such file or directory
```

---

## 2. Environment

Commands and raw values:

```text
$ date -u '+%Y-%m-%dT%H:%M:%SZ'
2026-11-21T09:14:10Z

$ uname -a
Linux runtime-pxnhkrqiyeijsprj-565f7c8ddf-q44rb 6.8.0-1055-gke #61-Ubuntu SMP Tue May 26 22:57:42 UTC 2026 x86_64 GNU/Linux

$ cat /etc/os-release
PRETTY_NAME="Debian GNU/Linux 13 (trixie)"
NAME="Debian GNU/Linux"
VERSION_ID="13"
VERSION="13 (trixie)"
VERSION_CODENAME=trixie
DEBIAN_VERSION_FULL=13.7
ID=debian

$ cc --version | head -1
cc (Debian 14.2.0-19) 14.2.0

$ g++ --version | head -1
g++ (Debian 14.2.0-19) 14.2.0

$ qemu-system-x86_64 --version | head -2
QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1)
Copyright (c) 2003-2025 Fabrice Bellard and the QEMU Project developers

$ ls -ld /lib/modules/*/build
lrwxrwxrwx 1 root root 47 Nov 12 09:30 /lib/modules/6.12.117+deb13-amd64/build -> ../../../src/linux-headers-6.12.117+deb13-amd64
lrwxrwxrwx 1 root root 37 May 26 18:41 /lib/modules/6.8.0-1055-gke/build -> /usr/src/linux-headers-6.8.0-1055-gke
```

Available tools: `cpio`, `gzip`, `file`, `timeout`, `make`, `python3`. `lspci` is **not installed**.

---

## 3. Read-only PCI and i915 evidence

Command:

```sh
for d in /sys/bus/pci/devices/*; do
  printf '%s vendor=%s device=%s class=%s driver=%s\n' \
    "$(basename "$d")" "$(cat "$d/vendor")" "$(cat "$d/device")" \
    "$(cat "$d/class")" "$(basename "$(readlink -f "$d/driver" 2>/dev/null)")"
done
```

Raw result:

```text
0000:00:00.0 vendor=0x8086 device=0x1237 class=0x060000 driver=driver
0000:00:01.0 vendor=0x8086 device=0x7110 class=0x060100 driver=driver
0000:00:01.3 vendor=0x8086 device=0x7113 class=0x068000 driver=driver
0000:00:03.0 vendor=0x1ae0 device=0x0042 class=0x020000 driver=gve
0000:00:04.0 vendor=0x1af4 device=0x1005 class=0x00ff00 driver=virtio-pci
0000:00:05.0 vendor=0x1ae0 device=0x001f class=0x010802 driver=nvme
```

The first three entries have no bound driver; `readlink -f` canonicalizes the nonexistent `driver` link, so `driver` is not a real driver name. The important fact is the absence of a display/GPU device.

Direct checks:

```text
$ test -e /sys/bus/pci/devices/0000:00:02.0 && echo present || echo absent
absent

$ grep -w i915 /proc/modules || echo absent
absent

$ ls -ld /dev/dri
ls: cannot access '/dev/dri': No such file or directory

$ command -v lspci || echo absent
absent
```

**Conclusion:** This container does not expose `0000:00:02.0`; i915 is not loaded; there is no DRM render node; `lspci` is unavailable. The audit therefore cannot prove the host i915 binding claim. It also found no physical GPU operation being attempted. This is an environment limitation, not evidence that the device was unbound by this audit.

---

## 4. Commands and exit codes

The following commands were executed. Logs were captured under `/tmp/stage3c_*.log`; the two repository logs are also listed. All exit codes are the shell exit codes.

| # | Command | Exit | Result |
|---:|---|---:|---|
| 1 | `git status --porcelain=v1` | 0 | clean; empty output |
| 2 | `make memory-manager-test` | 0 | PASS |
| 3 | `make host-memory-backend-test` | 0 | PASS |
| 4 | `make native-dma-backend-test` | 0 | SKIP; device absent |
| 5 | `make ppgtt-model-test` | 0 | PASS |
| 6 | `make pt-dma-adapter-test` | 0 | PASS |
| 7 | `make stage3a-test` | 0 | PASS (three tests) |
| 8 | `make stage3a-sanitize-test` | 0 | PASS (three ASan/UBSan tests) |
| 9 | `make kernel KDIR=/lib/modules/6.8.0-1055-gke/build` | 2 | FAIL: `gcc-13: not found` |
| 10 | `make kernel KDIR=/lib/modules/6.8.0-1055-gke/build CC=gcc` | 0 | PASS |
| 11 | `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build` | 0 | PASS |
| 12 | `make test` | 2 | 12 PASS, 1 SKIP; smoke test fails: no render node |
| 13 | `KDIR=/lib/modules/6.12.117+deb13-amd64/build IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 make qemu-dma-test` | 0 | PASS; QEMU exit status 33 |
| 14 | QEMU fault-injection test | N/A | no target or mode exists |
| 15 | Native backend sanitizer test | N/A | no target exists |

The QEMU runner writes `build/qemu-dma/run.log`. The committed historical evidence log is `tests/qemu-dma/evidence-2026-11-20-native-dma-backend.log`.

---

## 5. Raw test output excerpts

### 5.1 Native backend build/test — skipped, not passed

```text
$ make native-dma-backend-test
...
./build/native-dma-backend-test
SKIP: /dev/idw-edu-dma is not present; run under the QEMU EDU harness
```

Exit: `0`. This is a **skip**, not a native backend pass. The static test is executed with `--require-device` only inside the QEMU guest (`tests/qemu-dma/guest_init.c:127-135`).

### 5.2 Host backend

```text
$ make host-memory-backend-test
...
PASS: host backing allocates real pages and integrates with MemoryManager
```

Exit: `0`.

### 5.3 Memory manager

```text
$ make memory-manager-test
...
PASS: integrated memory allocation, VA, binding, submission, and recovery lifecycle
NOTE: backend is simulated; this is not physical GPU validation.
```

Exit: `0`.

### 5.4 PPGTT model

```text
$ make ppgtt-model-test
...
PASS: CPU-only GPU-VA allocator and Haswell Gen7.5 PPGTT model tests
NOTE: synthetic DMA addresses validate software structures only; no GPU mapping or hardware control was tested.
```

Exit: `0`.

### 5.5 DMA page-table adapter

```text
$ make pt-dma-adapter-test
...
PASS: DMA page-table adapter validation, rollback, sync, cleanup, and PPGTT integration
NOTE: synthetic DMA addresses and simulated provider; no device or GPU operation occurred.
```

Exit: `0`.

### 5.6 Stage 3A and sanitizers

```text
$ make stage3a-test
PASS: GGTT allocator, residency, binding, and teardown tests
PASS: Haswell GGTT directory, hardware sequence, and rollback tests
PASS: independent-device ownership, teardown, and recovery tests
```

Exit: `0`.

```text
$ make stage3a-sanitize-test
ASAN_OPTIONS=detect_leaks=1 ./build/sanitize/ggtt-allocator-test
PASS: GGTT allocator, residency, binding, and teardown tests
ASAN_OPTIONS=detect_leaks=1 ./build/sanitize/device-lifecycle-test
PASS: independent-device ownership, teardown, and recovery tests
ASAN_OPTIONS=detect_leaks=1 ./build/sanitize/haswell-gtt-hw-test
PASS: Haswell GGTT directory, hardware sequence, and rollback tests
```

Exit: `0`. This sanitizer target does **not** cover `native_dma_backend.cpp` or the kernel provider.

### 5.7 Kernel build — 6.8 default failure

```text
$ make kernel KDIR=/lib/modules/6.8.0-1055-gke/build
...
warning: the compiler differs from the one used to build the kernel
  The kernel was built by: x86_64-linux-gnu-gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
  You are using:
  CC [M]  .../kernel/idw_core_main.o
/bin/sh: 1: gcc-13: not found
make[3]: *** [scripts/Makefile.build:243: .../kernel/idw_core_main.o] Error 127
...
make: *** [Makefile:185: kernel] Error 2
```

Exit: `2`. The failure is a missing `gcc-13` in the environment, not a source compile error. Retrying with the available compiler:

```text
$ make kernel KDIR=/lib/modules/6.8.0-1055-gke/build CC=gcc
...
LD [M] .../kernel/idw_edu_dma_test.ko
LD [M] .../kernel/idw_core.ko
...
```

Exit: `0`.

### 5.8 Kernel build — 6.12

```text
$ make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build
make -C /lib/modules/6.12.117+deb13-amd64/build M=/workspace/project/intel_win_onlinux/kernel modules
...
```

Exit: `0`.

### 5.9 Aggregate software suite — stops at missing render node

```text
$ make test
...
PASS: page-table backend capability and device-scope contract tests
PASS: i915 GEM resource backend state, binding, and cleanup tests
PASS: i915-assisted memory lifecycle, GPU write, and recovery tests
PASS: Haswell no-op and store-dword encoding, bounds, failure, and cleanup tests
PASS: integrated memory allocation, VA, binding, submission, and recovery lifecycle
PASS: host backing allocates real pages and integrates with MemoryManager
SKIP: /dev/idw-edu-dma is not present; run under the QEMU EDU harness
PASS: CPU-only GPU-VA allocator and Haswell Gen7.5 PPGTT model tests
PASS: DMA page-table adapter validation, rollback, sync, cleanup, and PPGTT integration
PASS: GGTT allocator, residency, binding, and teardown tests
PASS: Haswell GGTT directory, hardware sequence, and rollback tests
PASS: independent-device ownership, teardown, and recovery tests
PASS: CLI rejects overflowing, partial, signed, and unexpected arguments before device access.
./tests/smoke.sh
IDW — Intel GPU Properties
==========================
No Intel DRM render node found.
Check: lspci -nnk | grep -A3 -i vga and ls -l /dev/dri
FAIL: no Intel render node found
make: *** [Makefile:166: test] Error 2
```

Exit: `2`. The software targets before smoke pass; the smoke target fails because the container has no Intel render node. This is an environment limitation, not a QEMU test failure.

### 5.10 QEMU EDU happy path

Command:

```sh
KDIR=/lib/modules/6.12.117+deb13-amd64/build \
IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 \
make qemu-dma-test
```

Exit: `0`. QEMU log header:

```text
QEMU version: QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1)
Acceleration: TCG (KVM not used)
Guest kernel: /boot/vmlinuz-6.12.117+deb13-amd64 (6.12.117+deb13-amd64)
Device: EDU PCI 1234:11e8, dma_mask=0xffffffffff
Command: qemu-system-x86_64 -machine pc,accel=tcg -device edu,dma_mask=0xffffffffff
```

Relevant guest output:

```text
IDW_QEMU_DMA: rejected provider owner mismatch
IDW_QEMU_DMA: rejected 32-bit mask; accepted configured 40-bit mask
IDW_QEMU_DMA: page-table DMA layout verified bytes=4096 first=000002b1 last=003ff2b1
IDW_QEMU_DMA: page-table malformed descriptors and unsupported mapping rejected
IDW_QEMU_DMA: page-table adapter cleanup verified
IDW_QEMU_DMA: round-trip source=0x00000000023e8000 destination=0x00000000023e7000 bytes=4096 first=833879b9
IDW_QEMU_DMA: invalid scope, length, DMA address and cookie rejected
IDW_QEMU_DMA: 32 device DMA read/write cycles verified
IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified
IDW_QEMU_DMA: probe passed; retained one page to verify remove cleanup
IDW_QEMU_DMA: running native DMA backend through MemoryManager
PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup
IDW_QEMU_DMA: remove freed outstanding provider allocations=yes result=0
IDW_QEMU_DMA: driver removal completed successfully
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
QEMU exit status: 33
PASS: QEMU EDU performed device-side DMA read/write, page-table DMA visibility, resource backing DMA, and provider removal cleanup.
```

The line `remove freed outstanding provider allocations=yes result=0` is evidence that the test intentionally frees outstanding allocations during removal. The audit treats this as a safety defect, not a success criterion.

### 5.11 QEMU fault injection — unavailable

There is no QEMU fault-injection target or mode. The only QEMU target is:

```text
qemu-dma-test:
    ./tests/qemu-dma/run.sh
```

`tests/qemu-dma/run.sh:112-119` checks only the happy-path success strings and exit 33. `tests/qemu-dma/guest_init.c:127-135` runs the happy native test and unloads the module. There is no failure injection for allocation, DMA mapping, PT allocation, PTE population, sync, mmap, removal, invalidation, or quarantine.

### 5.12 Native backend sanitizer — unavailable

`make stage3a-sanitize-test` builds and runs ASan/UBSan for `ggtt-allocator-test`, `device-lifecycle-test`, and `haswell-gtt-hw-test`. The Makefile has no target that sanitizes `native_dma_backend.cpp`, `idw_dma_memory.c`, or `idw_edu_dma_test_main.c`.

---

## 6. Evidence hashes

All hashes below were computed with `sha256sum` from the actual files at audit time. `/tmp` logs are ephemeral; the hash is the audit-time identity.

| File | Lines | SHA-256 |
|---|---:|---|
| `/tmp/stage3c_preflight.txt` | 70 | `b5703557adb77a616fc6b87d546dbbc68b278eadeecae6c187392584ab928765` |
| `/tmp/stage3c_memory-manager-test.log` | 9 | `d20e0a5ffd44bf1e5117f5b1053c272811f0cdcd1e6ad3f3c8317dd708e5a905` |
| `/tmp/stage3c_host-memory-backend-test.log` | 9 | `e649bccf1018fa217094be42b4e5d76d1ffecf611b0374c16bb5a7d949ba0cbb` |
| `/tmp/stage3c_native-dma-backend-test.log` | 15 | `e2ededb176db70bf5d1b836131a29eab580ee653c777838a412199df7d48647e` |
| `/tmp/stage3c_ppgtt-model-test.log` | 7 | `3d3b82b34484d9e5906e6496c2d573673fb20f71adb657402e22802961429218` |
| `/tmp/stage3c_pt-dma-adapter-test.log` | 8 | `2507dd771d292c383ee62a3cc4396a0806825f91521dfc36169e3dbf911db812` |
| `/tmp/stage3c_stage3a-test.log` | 16 | `0b0a960ce33bb5499177a32fb0892f8eee04b2b6deb602230ae4c57c05770a1b` |
| `/tmp/stage3c_stage3a-sanitize-test.log` | 19 | `73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294` |
| `/tmp/stage3c_kernel-6.8.log` | 12 | `985a3aae9c28c780fbae2b0bfb85c3473622fe62404f8a12fa8de79767a08271` |
| `/tmp/stage3c_kernel-6.8-cc-gcc.log` | 29 | `8efc37dcb117ba4b96cd6c59b7afcb26020d6f03d99ac4779b412488f5db8539` |
| `/tmp/stage3c_kernel-6.12.log` | 3 | `32cf1e804abab95595e1c97f43c5d86316b6745013669e8c9013bc8e6439e511` |
| `/tmp/stage3c_make-test.log` | 161 | `ea441f0f24d4e80b4e0366162ebee13ffd640eca3bb885edae9bd97d6134398f` |
| `/tmp/stage3c_qemu-happy.log` | 405 | `ea0193f3e51532d1a9b293d9c0124f2543719424aaf09dc2225c0f434fe57aea` |
| `build/qemu-dma/run.log` | 366 | `21abe79dee2408e4549b748459bd8d6077b03c84a7dc95ebc6d2884b8136dc73` |
| `tests/qemu-dma/evidence-2026-11-20-native-dma-backend.log` | 366 | `0c7ac5cb92c9aed9acaaf6e8bc2709ed584541bbff981a90543692258328eedf` |

The committed evidence log hash `0c7ac5...` matches the value documented in `tests/qemu-dma/README.md:87-88`. The fresh `build/qemu-dma/run.log` is a separate run and has a different hash because the QEMU run contains different DMA addresses/timings. Both report the same pass conditions and QEMU status 33.

---

## 7. Five evidence buckets

### Bucket 1 — QEMU happy-path evidence

**Present.** QEMU 10.0.13 TCG, guest kernel `6.12.117+deb13-amd64`, EDU PCI `1234:11e8`, `dma_mask=0xffffffffff`, QEMU exit status 33. The guest reports page-table DMA, 32 DMA cycles, 3-page resource backing, PPGTT PTE consumption, sync, native backend lifecycle, and provider removal. This proves virtual-device DMA and CPU-model address plumbing only.

### Bucket 2 — QEMU fault-injection evidence

**Empty.** No fault-injection target, mode, or test exists. Missing cases include first/middle/last backing allocation, first/middle DMA mapping, mapping error, misaligned/out-of-range address, PT allocation failure, PTE population failure, page-table sync failure, userspace mmap failure, provider removal with outstanding resources, quiescence uncertainty, rollback, and quarantine.

### Bucket 3 — CPU-model/software evidence

**Present.** `make ppgtt-model-test`, `make pt-dma-adapter-test`, `make memory-manager-test`, `make host-memory-backend-test`, `make stage3a-test`, and `make stage3a-sanitize-test` pass. These are synthetic/simulated or host-memory tests. They do not prove physical GPU behavior. The model tests cover PDE/PTE encoders, 39-bit/40-bit boundaries, sync failure, invalidation failure, and rollback; the adapter tests cover simulated provider failures.

### Bucket 4 — i915-assisted physical execution evidence

**Empty.** The i915-related software tests in `make test` explicitly say “ioctl callbacks are simulated” and “DRM ioctls are simulated; no physical GPU was used.” No i915-assisted physical execution occurred.

### Bucket 5 — Independent physical GPU validation evidence

**Empty.** No physical Haswell GPU is exposed in this container. No physical BAR, MMIO, DMA, GPU submission, GGTT publication, PPGTT activation, or page walk was performed. The i915 binding of `0000:00:02.0` is unverified here.

---

## 8. QEMU virtual DMA vs physical Haswell

| Question | Answer |
|---|---|
| Was QEMU EDU device-side DMA performed? | Yes, in TCG, with a virtual `1234:11e8` device. |
| Were real Linux DMA addresses used in the guest? | Yes, guest DMA addresses from the EDU provider. |
| Did a physical Haswell GPU consume those addresses? | No. |
| Was a physical Haswell page walk performed? | No. |
| Was physical Haswell PPGTT activated? | No. |
| Was physical GGTT published? | No. |
| Was physical BAR/MMIO accessed? | No. |
| Was a physical ring submitted? | No. |
| Was PCI ownership of `0000:00:02.0` changed? | No operation was attempted; the device is not exposed here, so binding is unverified. |

---

## 9. Skips and unavailable evidence

| Item | Status | Reason |
|---|---|---|
| `make native-dma-backend-test` | SKIP | `/dev/idw-edu-dma` absent on host; QEMU guest uses `--require-device` |
| `make test` smoke test | FAIL (environment) | no Intel DRM render node; no `/dev/dri` |
| `make kernel` with 6.8 default | FAIL (toolchain) | kernel headers request `gcc-13`; only gcc 14 installed |
| `make kernel` with 6.8 and `CC=gcc` | PASS | explicit available compiler |
| QEMU fault injection | NOT AVAILABLE | no target/mode |
| Native backend sanitizer | NOT AVAILABLE | no target |
| Physical Haswell validation | NOT AVAILABLE | no GPU exposed; prohibited by audit scope |
| i915 binding evidence | INSUFFICIENT | no `0000:00:02.0`, no i915 module, no `lspci` |

---

## 10. Integrity statement

No implementation code, test, build script, UAPI definition, or configuration was modified. The only repository changes made by this audit are the two requested documentation files:

- `docs/design/STAGE3C_17_POINT_PR_AUDIT.md`
- `tests/qemu-dma/STAGE3C_AUDIT_EVIDENCE.md`

No commit, push, merge, PR approval, or PR close was performed. No physical GPU operation was performed.
