# QEMU EDU DMA validation

This opt-in test exercises IDW's real Linux `dma_alloc_coherent()` provider
inside a guest against QEMU's upstream EDU PCI device (`1234:11e8`). EDU is a
virtual bus-master device: its DMA engine transfers data between guest RAM and
its internal 4 KiB buffer. This is device-side DMA in QEMU, not a software
callback pretending to perform DMA and not proof of Haswell DMA behavior.
The register and transfer behavior follows the
[QEMU EDU device specification](https://github.com/qemu/qemu/blob/master/docs/specs/edu.rst).

The guest PCI driver binds only to EDU. It sets 40-bit streaming/coherent DMA
masks, initializes the IDW provider with the bound PCI device and its own
module identity, exercises owner/mask/descriptor rejection, performs 32
device-side read-then-write round trips through coherent pages, checks exact
CPU readback, and leaves one allocation outstanding for PCI remove/provider
teardown to free. DMA write/read barriers surround the coherent-buffer
transfers. The guest runner unloads the test driver so its PCI `remove` callback
and provider cleanup are part of the success condition.

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

## Evidence boundary

A successful run must show the `IDW_QEMU_DMA: PASS` serial marker and QEMU
debug-exit status 33. The guest log must also report verified DMA cycles and
that remove freed an outstanding provider page. The guest init process emits
the final pass marker and debug-exit signal only after the driver-unload
syscall returns. Compilation, software tests,
or a QEMU boot without those signals are not a pass. Even a successful QEMU
run validates Linux DMA-provider lifecycle and QEMU's EDU bus-master model
only; Stage 2's physical Haswell/IDW-owned DMA proof remains separate.
