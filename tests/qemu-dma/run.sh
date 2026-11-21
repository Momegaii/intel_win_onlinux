#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
KDIR=${KDIR:-/lib/modules/$(uname -r)/build}
KERNEL=${IDW_QEMU_KERNEL:-/boot/vmlinuz-linux}
QEMU=${QEMU:-qemu-system-x86_64}
BUILD_ONLY=false

if [[ ${1:-} == --build-only ]]; then
    BUILD_ONLY=true
elif [[ $# -ne 0 ]]; then
    printf 'Usage: %s [--build-only]\n' "$0" >&2
    exit 2
fi

for tool in cc cpio gzip file; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'ERROR: required tool not found: %s\n' "$tool" >&2
        exit 2
    fi
done
if [[ ! -d "$KDIR" || ! -f "$KDIR/Makefile" ]]; then
    printf 'ERROR: kernel build tree not found: %s\n' "$KDIR" >&2
    exit 2
fi
if [[ ! -r "$KERNEL" ]]; then
    printf 'ERROR: guest kernel image not readable: %s\n' "$KERNEL" >&2
    exit 2
fi
if [[ $(uname -m) != x86_64 ]]; then
    printf 'ERROR: this guest test harness currently supports x86_64 only\n' >&2
    exit 2
fi

KERNEL_RELEASE=$(make -s -C "$KDIR" kernelrelease)
IMAGE_RELEASE=$(file -b "$KERNEL" | sed -n 's/.*version \([^ ]*\).*/\1/p')
if [[ -z "$IMAGE_RELEASE" || "$IMAGE_RELEASE" != "$KERNEL_RELEASE" ]]; then
    printf 'ERROR: guest kernel (%s) does not match module tree (%s)\n' \
        "${IMAGE_RELEASE:-unidentified}" "$KERNEL_RELEASE" >&2
    printf 'Set IDW_QEMU_KERNEL and KDIR to a matching kernel pair.\n' >&2
    exit 2
fi

make -C "$ROOT" kernel KDIR="$KDIR"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/idw-qemu-dma.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root/modules" "$ROOT/build/qemu-dma"
cp "$ROOT/kernel/idw_core.ko" "$WORK/root/modules/"
cp "$ROOT/kernel/idw_edu_dma_test.ko" "$WORK/root/modules/"
make -C "$ROOT" native-dma-backend-test-static native-dma-fault-test-static
cp "$ROOT/build/native-dma-backend-test-static" \
    "$WORK/root/native-dma-backend-test"
cp "$ROOT/build/native-dma-fault-test-static" \
    "$WORK/root/native-dma-fault-test"
cc -static -O2 -Wall -Wextra -Werror \
    "$ROOT/tests/qemu-dma/guest_init.c" -o "$WORK/root/init"
chmod 0755 "$WORK/root/init" "$WORK/root/native-dma-backend-test" \
    "$WORK/root/native-dma-fault-test"
(
    cd "$WORK/root"
    find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -n
) > "$ROOT/build/qemu-dma/initramfs.cpio.gz"

if [[ "$BUILD_ONLY" == true ]]; then
    printf 'Build-only complete: guest modules and initramfs are ready; no DMA test was run.\n'
    exit 0
fi

for tool in "$QEMU" timeout tee; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'ERROR: required tool not found: %s\n' "$tool" >&2
        if [[ "$tool" == qemu-system-x86_64 ]]; then
            printf 'Arch package providing it: qemu-system-x86\n' >&2
        fi
        exit 2
    fi
done

LOG="$ROOT/build/qemu-dma/run.log"
printf 'Running QEMU EDU PCI DMA test with TCG (no KVM): %s\n' "$QEMU"
printf 'Guest kernel: %s (%s)\n' "$KERNEL" "$KERNEL_RELEASE"
{
    printf 'QEMU version: '
    "$QEMU" --version | sed -n '1p'
    printf 'Acceleration: TCG (KVM not used)\n'
    printf 'Guest kernel: %s (%s)\n' "$KERNEL" "$KERNEL_RELEASE"
    printf 'Device: EDU PCI 1234:11e8, dma_mask=0xffffffffff\n'
    printf 'Command: %s -machine pc,accel=tcg -device edu,dma_mask=0xffffffffff\n' \
        "$QEMU"
} > "$LOG"
set +e
# timeout puts QEMU in a separate process group. Detach stdin so the stdio
# serial backend cannot stop the guest with SIGTTIN when run from a TTY.
timeout --signal=TERM --kill-after=5 90 \
    "$QEMU" \
        -machine pc,accel=tcg \
        -m 256M \
        -kernel "$KERNEL" \
        -initrd "$ROOT/build/qemu-dma/initramfs.cpio.gz" \
        -append 'console=ttyS0,115200 rdinit=/init loglevel=7 panic=-1' \
        -nodefaults \
        -no-reboot \
        -display none \
        -monitor none \
        -serial stdio \
        -device edu,dma_mask=0xffffffffff \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        < /dev/null 2>&1 | tee -a "$LOG"
QEMU_STATUS=${PIPESTATUS[0]}
set -e
printf 'QEMU exit status: %d\n' "$QEMU_STATUS" | tee -a "$LOG"

if [[ $QEMU_STATUS -eq 33 ]] &&
   grep -q 'IDW_QEMU_DMA: page-table DMA layout verified' "$LOG" &&
   grep -q 'IDW_QEMU_DMA: 3-page resource backing DMA, 39-bit validation, PPGTT PTE consumption, sync, and cleanup verified' "$LOG" &&
   grep -q 'IDW_QEMU_DMA: backing allocation/mapping fault injection verified' "$LOG" &&
   grep -q 'IDW_QEMU_DMA: provider removal refused outstanding refs and recovered after release' "$LOG" &&
   grep -q 'PASS: native DMA backing, real DMA addresses, PPGTT translation, submission, and cleanup' "$LOG" &&
   grep -q 'PASS: native DMA backend fault injection, teardown retry, VMA lifetime, and ownership counters' "$LOG" &&
   grep -q 'IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed' "$LOG"; then
    printf 'PASS: QEMU EDU performed device-side DMA read/write, page-table DMA visibility, resource backing DMA, and provider removal cleanup.\n'
    exit 0
fi

printf 'FAIL: QEMU did not report complete DMA success (status=%d).\n' \
    "$QEMU_STATUS" >&2
printf 'Log: %s\n' "$LOG" >&2
exit 1
