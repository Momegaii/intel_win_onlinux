#!/usr/bin/env bash
set -euo pipefail
BIN="${IDWCTL:-./build/idwctl}"
if [[ ! -x "$BIN" ]]; then
  echo "Missing $BIN; run make first." >&2
  exit 2
fi
# Explicit hardware test: submits a no-op batch to Haswell through i915.
exec "$BIN" gpu-memory-test
