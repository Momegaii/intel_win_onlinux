#!/usr/bin/env bash
set -euo pipefail
BIN="${IDWCTL:-./build/idwctl}"
if [[ ! -x "$BIN" ]]; then
  echo "Missing $BIN; run make first." >&2
  exit 2
fi
# Explicit hardware test: writes only to its own managed GEM object on Haswell.
exec "$BIN" gpu-resource-write-test
