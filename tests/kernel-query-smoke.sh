#!/usr/bin/env bash
set -euo pipefail
BIN=${1:-"$(dirname "$0")/../build/idwctl"}
if [[ ! -e /dev/idw ]]; then
  echo "SKIP: /dev/idw is absent (load idw_core.ko to run this test)."
  exit 0
fi
if [[ ! -r /dev/idw || ! -w /dev/idw ]]; then
  echo "SKIP: /dev/idw is not accessible to this user (run with appropriate permissions)."
  exit 0
fi
"$BIN" kernel
