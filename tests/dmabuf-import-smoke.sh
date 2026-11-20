#!/usr/bin/env bash
set -euo pipefail
BIN="${1:-./build/idwctl}"
if [[ ! -x "$BIN" ]]; then
  echo "SKIP: idwctl not built"
  exit 0
fi
if [[ ! -e /dev/idw ]]; then
  echo "SKIP: /dev/idw unavailable (load the optional IDW module)"
  exit 0
fi
if [[ ! -r /dev/idw || ! -w /dev/idw ]]; then
  echo "SKIP: /dev/idw is not accessible to this user (run with appropriate permissions)."
  exit 0
fi
if ! "$BIN" dmabuf-test; then
  echo "FAIL: dma-buf import test failed" >&2
  exit 1
fi
