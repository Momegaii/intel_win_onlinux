#!/usr/bin/env bash
set -euo pipefail
BIN="${IDWCTL:-./build/idwctl}"
if [[ ! -x "$BIN" ]]; then
  echo "Missing $BIN; run make first." >&2
  exit 2
fi
# Explicit hardware test: requires /dev/idw plus an i915 render node.
exec "$BIN" gpu-resource-test
