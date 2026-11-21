# QEMU EDU DMA validation

This opt-in test exercises IDW's real Linux `dma_alloc_coherent()` provider
inside a guest against QEMU's upstream EDU PCI device (`1234:11e8`). EDU is a
virtual bus-master device: its DMA engine transfers data between guest RAM and
its internal 4 KiB buffer. This is device-side DMA in QEMU, not a software
callback pretending to perform DMA and not proof of Haswell DMA behavior.
The register and transfer behavior follows the
[QEMU EDU device specification](https://github.com/qemu/qemu/blob/master/docs/specs/edu.rst).

The guest PCI driver binds only to EDU. It sets a 39-bit streaming DMA mask and
a 40-bit coherent DMA mask, initializes the IDW provider with the bound PCI device and its own
module identity, exercises owner/mask/descriptor rejection, copies a populated
Gen7.5 page table through EDU and verifies its exact layout, performs 32
device-side read-then-write round trips through coherent pages, checks exact
CPU readback, and releases every allocation before PCI remove/provider
teardown. DMA write/read barriers surround the coherent-buffer transfers. The
guest runner unloads the test driver so its PCI `remove` callback and provider
cleanup are part of the success condition; removal fails closed and quarantines
if any page, resource, or VMA reference remains.

After the kernel self-test, the guest runs the static
`native-dma-backend-test`. It uses `NativeDmaMemoryBackend` through
`MemoryManager`: allocate resource backing, map it, bind it at a fixed GPU VA,
read the real DMA segments, translate through the Haswell Gen7.5 PPGTT model,
write/read a dword from the CPU, submit a dword write and wait, reject destroy
while bound, unbind, destroy, and close. The test must print
`PASS: native DMA backing, real DMA addresses, PPGTT translation, submission,
and cleanup`.

The guest then runs `native-dma-fault-test`, which injects backing allocation,
page-table allocation, mmap, sync, bind, unbind, and destroy failures. It checks
quarantine transitions, retry after partial unbind, recovery after a failed
bind, and that all ownership counters return to zero only after model and
provider teardown. It must print `PASS: native DMA backend fault injection,
retry, and ownership counters`.

## Run

The runner uses TCG only; it does not use KVM. It defaults to the current
kernel's module build tree and `/boot/vmlinuz-linux`; those must be a matching
x86_64 pair. Override them with `KDIR=/path/to/build` and
`IDW_QEMU_KERNEL=/path/to/bzImage` if needed.

```sh
make qemu-dma-test
```

The target builds the guest modules and a small static initramfs locally, then
launches QEMU with `-machine pc,accel=tcg` and
`-device edu,dma_mask=0xffffffffff`. It uses QEMU's `isa-debug-exit` device to
distinguish a completed test from a timeout or guest boot failure. No package
is installed by the target.

To validate only the guest-module/initramfs build prerequisites without
starting a VM, run `tests/qemu-dma/run.sh --build-only`. This is build evidence
only and must not be reported as a DMA test pass.

On Arch Linux, the QEMU executable is supplied by package `qemu-system-x86`.
Installation is intentionally a separate, user-approved step:

```sh
sudo pacman -S --needed qemu-system-x86
```

The test does not bind, unbind, or otherwise alter the host's physical GPU.
Host KVM support is not required, although TCG execution is slower.

The runner detaches QEMU stdin from the terminal. `timeout` places QEMU in a
separate process group, and the `stdio` serial backend can otherwise stop the
guest with `SIGTTIN` when the test is launched from a terminal.

## Recorded evidence

On 2026-11-20 the corrected page-table path passed in a Debian 13 container
with QEMU 10.0.13 TCG and a Debian `6.12.117+deb13-amd64` guest:

```text
IDW_QEMU_DMA: page-table DMA layout verified bytes=4096 first=000002b1 last=003ff2b1
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
QEMU exit status: 33
```

The same run now also executes the native backend:

```text
IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified
PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup
IDW_QEMU_DMA: remove freed outstanding provider allocations=yes result=0
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
QEMU exit status: 33
```

The full serial log is
`tests/qemu-dma/evidence-2026-11-20-native-dma-backend.log` (SHA-256
`0c7ac5cb92c9aed9acaaf6e8bc2709ed584541bbff981a90543692258328eedf`). The
earlier page-table-only log is
`tests/qemu-dma/evidence-2026-11-20-tcg-page-table-corrected.log` (SHA-256
`393140205e06fbb6b9e3eaad161eb6181fed9ca9c3274e11108df0693836f22a`).
The earlier `evidence-2026-11-20-tcg-page-table.log` used the superseded
shared PDE/PTE encoder and is retained as historical evidence only.

## Evidence boundary

A successful run must show the `IDW_QEMU_DMA: PASS` serial marker and QEMU
debug-exit status 33. The guest log must also report verified page-table DMA
layout visibility, verified DMA cycles, the native backend `PASS` marker, and
that remove freed an outstanding provider page. The guest init process emits
the final pass marker and debug-exit signal only after the driver-unload
syscall returns. Compilation, software tests,
or a QEMU boot without those signals are not a pass. Even a successful QEMU
run validates Linux DMA-provider lifecycle and QEMU's EDU bus-master model
only; Stage 2's physical Haswell/IDW-owned DMA proof remains separate.
