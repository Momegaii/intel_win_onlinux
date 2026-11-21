#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
FORCE=false
if [[ ${1:-} == --force ]]; then
    FORCE=true
    shift
fi
OUT=${1:-"$ROOT/tests/physical/evidence-$(date -u +%Y%m%dT%H%M%SZ)"}
TARGET=0000:00:02.0
TARGET_VENDOR=0x8086
TARGET_DEVICE=0x0412

if [[ -e "$OUT" && ! -d "$OUT" ]]; then
    printf 'ERROR: output path exists and is not a directory: %s\n' "$OUT" >&2
    exit 2
fi
if [[ -d "$OUT" && -n "$(ls -A "$OUT")" && "$FORCE" != true ]]; then
    printf 'ERROR: output directory is not empty: %s\n' "$OUT" >&2
    printf 'Use a new directory or pass --force.\n' >&2
    exit 2
fi
mkdir -p "$OUT"

{
    printf 'date_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'uname=%s\n' "$(uname -a)"
    printf 'architecture=%s\n' "$(uname -m)"
    printf 'virtualization=%s\n' "$(systemd-detect-virt 2>/dev/null || echo unknown)"
    printf 'hypervisor_type=%s\n' "$(cat /sys/hypervisor/type 2>/dev/null || echo none)"
    printf 'kernel_cmdline=%s\n' "$(cat /proc/cmdline)"
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        printf 'os=%s\n' "${PRETTY_NAME:-unknown}"
    fi
    grep -m1 'model name' /proc/cpuinfo || true
} > "$OUT/environment.log" 2>&1

{
    if command -v lspci >/dev/null 2>&1; then
        lspci -nnk
    else
        printf 'LSPCI_UNAVAILABLE\n'
    fi
} > "$OUT/lspci.log" 2>&1

{
    for dev in /sys/bus/pci/devices/*; do
        [[ -e "$dev" ]] || continue
        name=$(basename "$dev")
        vendor=$(cat "$dev/vendor" 2>/dev/null || echo unknown)
        device=$(cat "$dev/device" 2>/dev/null || echo unknown)
        class=$(cat "$dev/class" 2>/dev/null || echo unknown)
        driver=$(basename "$(readlink -f "$dev/driver" 2>/dev/null)" 2>/dev/null || true)
        printf '%s vendor=%s device=%s class=%s driver=%s\n' \
            "$name" "$vendor" "$device" "$class" "${driver:-none}"
    done
} > "$OUT/pci.log" 2>&1

target_present=false
target_vendor=
target_device=
target_driver=

{
    if [[ -d "/sys/bus/pci/devices/$TARGET" ]]; then
        target_present=true
        target_vendor=$(cat "/sys/bus/pci/devices/$TARGET/vendor" 2>/dev/null || echo unknown)
        target_device=$(cat "/sys/bus/pci/devices/$TARGET/device" 2>/dev/null || echo unknown)
        target_driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$TARGET/driver" 2>/dev/null)" 2>/dev/null || true)
        printf 'TARGET_GPU_PRESENT: %s vendor=%s device=%s driver=%s\n' \
            "$TARGET" "$target_vendor" "$target_device" "${target_driver:-none}"
        if command -v lspci >/dev/null 2>&1; then
            lspci -nnk -s 00:02.0 || true
        fi
        printf 'sysfs_vendor=%s\n' "$target_vendor"
        printf 'sysfs_device=%s\n' "$target_device"
        printf 'sysfs_driver=%s\n' "${target_driver:-none}"
    else
        printf 'TARGET_GPU_NOT_PRESENT: %s absent\n' "$TARGET"
    fi
} > "$OUT/target.log" 2>&1

{
    if [[ -d /dev/dri ]]; then
        ls -l /dev/dri
    else
        printf 'NO_DEV_DRI\n'
    fi
    ls -d /sys/class/drm/* 2>/dev/null || true
} > "$OUT/drm.log" 2>&1

{
    if grep -qw i915 /proc/modules 2>/dev/null; then
        grep -w i915 /proc/modules
    else
        printf 'I915_NOT_LOADED\n'
    fi
    if [[ -d /sys/module/i915 ]]; then
        printf 'I915_SYSFS_PRESENT\n'
    else
        printf 'I915_SYSFS_ABSENT\n'
    fi
    if [[ -e /dev/dri/renderD128 ]]; then
        printf 'RENDER_NODE_PRESENT\n'
    else
        printf 'RENDER_NODE_ABSENT\n'
    fi
    if [[ -d /sys/kernel/iommu_groups ]]; then
        printf 'IOMMU_GROUPS=%s\n' "$(find /sys/kernel/iommu_groups -mindepth 1 -maxdepth 1 -type d | wc -l)"
    else
        printf 'IOMMU_GROUPS=0\n'
    fi
    if [[ -d /dev/vfio ]]; then
        ls -l /dev/vfio
    else
        printf 'NO_DEV_VFIO\n'
    fi
} > "$OUT/modules.log" 2>&1

{
    printf 'target_present=%s\n' "$target_present"
    printf 'target_vendor=%s\n' "${target_vendor:-unknown}"
    printf 'target_device=%s\n' "${target_device:-unknown}"
    printf 'target_driver=%s\n' "${target_driver:-none}"
} > "$OUT/summary.txt"

result=
if [[ "$target_present" != true ]]; then
    result=TARGET_GPU_NOT_PRESENT
elif [[ "$target_vendor" != "$TARGET_VENDOR" || "$target_device" != "$TARGET_DEVICE" ]]; then
    result=TARGET_GPU_MISMATCH
elif [[ "$target_driver" != i915 ]]; then
    result=TARGET_GPU_NOT_BOUND_TO_I915
elif [[ ! -e /dev/dri/renderD128 ]]; then
    result=RENDER_NODE_ABSENT
else
    result=PASS
fi
printf 'result=%s\n' "$result" >> "$OUT/summary.txt"
printf 'PHYSICAL_EVIDENCE_DIR=%s\n' "$OUT"
printf 'PHYSICAL_EVIDENCE_RESULT=%s\n' "$result"

case "$result" in
    PASS) exit 0 ;;
    TARGET_GPU_NOT_PRESENT) exit 2 ;;
    TARGET_GPU_MISMATCH) exit 3 ;;
    TARGET_GPU_NOT_BOUND_TO_I915) exit 4 ;;
    RENDER_NODE_ABSENT) exit 5 ;;
    *) exit 6 ;;
esac
