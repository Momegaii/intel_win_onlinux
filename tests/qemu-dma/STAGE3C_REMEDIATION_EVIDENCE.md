# Stage 3C — Remediation Evidence

**Date:** 2026-11-20 UTC
**Environment:** Debian GNU/Linux 13 container; kernel headers `6.12.117+deb13-amd64`; QEMU EDU TCG (no KVM)
**Baseline audited:** `fa2e91879b02526b4a31615ddf19561443d8e3cc`
**Remediation report:** `docs/design/STAGE3C_REMEDIATION_REPORT.md`

---

## 1. Build and non-QEMU tests

Command:

```sh
make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build
make native-dma-backend-test native-dma-fault-test \
     native-dma-backend-test-static native-dma-fault-test-static \
     memory-manager-test ppgtt-model-test pt-dma-adapter-test abi-test
make stage3a-sanitize-test
```

The second remediation pass adds VMA fork/split/multi-mapping coverage,
page-table VMA coverage, resource unmap/free retry, page-table free retry,
unbind sync-failure retry, and destroy-after-unmap retry. The local native
tests still skip without the EDU device; their PASS lines below are from QEMU.

Observed:

```text
Kernel module build: exit 0
SKIP: /dev/idw-edu-dma is not present; run under the QEMU EDU harness
PASS: integrated memory allocation, VA, binding, submission, and recovery lifecycle
PASS: CPU-only GPU-VA allocator and Haswell Gen7.5 PPGTT model tests
PASS: DMA page-table adapter validation, rollback, sync, cleanup, and PPGTT integration
PASS: GGTT allocator, residency, binding, and teardown tests
PASS: independent-device ownership, teardown, and recovery tests
PASS: Haswell GGTT directory, hardware sequence, and rollback tests
```

The local native and fault tests skip because the EDU device exists only inside
the QEMU guest. Their PASS lines are required in the QEMU run below.

Full `make test` was also run. Every software test passed, then the smoke
script stopped at the expected environmental check:

```text
PASS: CLI rejects overflowing, partial, signed, and unexpected arguments before device access.
No Intel DRM render node found.
FAIL: no Intel render node found
make: *** [Makefile:189: test] Error 2
```

This is the same missing physical GPU limitation recorded for Check 15.

Log hashes:

```text
7049243fd20b254a4582874269b7358c0198ab75751205bdc4735788f0d44009  evidence-2026-11-20-stage3c-kernel-build-pass2.log
f6767f5522700b0bbf31ba789502d475f2786b9554d7979a31bc85108cc505fd  evidence-2026-11-20-stage3c-tests-pass2.log
73310280e878c2d36d1833f3790c702adabc8f01b060fa3d325509ea798ac294  evidence-2026-11-20-stage3c-sanitize-pass2.log
27ebc1362554465bbece3e88d8e4b77f79cab78425b6d9c21312c33ca2632e76  evidence-2026-11-20-stage3c-make-test-pass2.log
```

---

## 2. QEMU EDU run

Command:

```sh
KDIR=/lib/modules/6.12.117+deb13-amd64/build \
IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 \
make qemu-dma-test
```

Runner metadata:

```text
Running QEMU EDU PCI DMA test with TCG (no KVM): qemu-system-x86_64
```

Guest excerpts:

```text
IDW_QEMU_DMA: rejected 32-bit mask; accepted 39-bit streaming and 40-bit coherent masks
IDW_QEMU_DMA: page-table DMA layout verified bytes=4096 first=000002b1 last=003ff2b1
IDW_QEMU_DMA: 32 device DMA read/write cycles verified
IDW_QEMU_DMA: backing allocation/mapping fault injection verified
IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified
IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release
IDW_QEMU_DMA: probe passed; no outstanding provider pages retained
IDW_QEMU_DMA: running native DMA backend through MemoryManager
PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup
IDW_QEMU_DMA: running native DMA fault injection and retry tests
PASS: native DMA backend fault injection, teardown retry, VMA lifetime, and ownership counters
IDW_QEMU_DMA: remove released all provider resources result=0
IDW_QEMU_DMA: driver removal completed successfully
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
QEMU exit status: 33
PASS: QEMU EDU performed device-side DMA read/write, page-table DMA visibility, resource backing DMA, and provider removal cleanup.
```

Log hash:

```text
a399119a2d9bd5b606fbb7af34ad7bc492a7e4cd959b405a138f18780be777da  evidence-2026-11-20-stage3c-remediation-pass2.log
```

---

## 3. What the QEMU run proves

- 39-bit streaming and 40-bit coherent masks were accepted; the 32-bit request
  was rejected.
- The page-table DMA layout and 32 device-side DMA cycles passed.
- Backing allocation and mapping fault injection ran inside the probe; the
  probe still retained exactly the live resources and released every page.
- The native backend happy path and fault-injection path both passed.
- The resource VMA test mapped three pages, closed one fd, verified the mapping
  still worked, forked a child that shared the VMA (`vma_refs == 2`), and
  verified the parent retained `vma_refs == 1` after the child unmapped.
- The same test created a second mapping (`vma_refs == 2`), rejected `FREE`
  with `-EBUSY`, then split the first VMA by unmapping its middle page
  (`vma_refs == 2` for the two remaining halves), unmapped both halves, and
  freed only after `vma_refs == 0`.
- The page-table VMA test repeated the close, fork, second mapping, `FREE_PT`
  rejection, and final release sequence.
- Resource unmap and free faults each returned `-EIO` without losing the
  resource, and a retry after clearing the fault freed it. A page-table free
  fault similarly retained the PT page until retry.
- An unbind sync failure cleared PTEs, kept the VA reservation, quarantined the
  resource, and recovered through `recover_binding()` after the fault cleared.
  A destroy failure after `munmap()` retained `dma_unmapped` progress and
  completed on retry.
- The probe's provider-removal self-test refused `idw_dma_provider_fini()`
  while resource, page, and VMA references were live, then succeeded after
  release.
- PCI remove succeeded only after all page, resource, and VMA references were
  released.

## 4. What it does not prove

- No physical Haswell BAR, MMIO, GGTT, ring, PPGTT activation, or DMA was
  performed.
- The page walk is the CPU model, not silicon.
- Physical i915 binding evidence is still unavailable in this container. See
  Check 15 in the remediation report.
