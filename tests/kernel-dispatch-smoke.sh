#!/usr/bin/env bash
set -euo pipefail

CORE=${1:-"$(dirname "$0")/../kernel/idw_core.ko"}
TEST=${2:-"$(dirname "$0")/../kernel/idw_test_miniport.ko"}

if [[ $EUID -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 2
fi
[[ -f "$CORE" && -f "$TEST" ]] || { echo "Build kernel modules first (make kernel)." >&2; exit 2; }
cleanup() {
  rmmod idw_test_miniport 2>/dev/null || true
  rmmod idw_core 2>/dev/null || true
}
trap cleanup EXIT
insmod "$CORE"
insmod "$TEST"
dmesg | tail -n 80 | grep -E 'idw(_test_miniport)?:' || true
rmmod idw_test_miniport
rmmod idw_core
trap - EXIT
echo "IDW callback lifecycle smoke test passed. Allocation is metadata-only; no GPU was touched."
