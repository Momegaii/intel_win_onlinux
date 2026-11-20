#!/usr/bin/env bash
set -euo pipefail
BIN="${IDWCTL:-./build/idwctl}"
if [[ ! -x "$BIN" ]]; then
  echo "Missing $BIN; run make first." >&2
  exit 2
fi
# Explicit hardware test: submits harmless batches through i915 on Haswell.
exec "$BIN" gpu-memory-manager-test
