#!/usr/bin/env bash
set -euo pipefail
BIN="${IDWCTL:-./build/idwctl}"
if [[ ! -x "$BIN" ]]; then
  echo "Missing $BIN; run make first." >&2
  exit 2
fi
# This is an explicit hardware test; never run it as part of the generic smoke suite.
exec "$BIN" gpu-submit "${IDW_GPU_SUBMIT_COUNT:-1}"
