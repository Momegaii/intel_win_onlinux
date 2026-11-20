#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ ! -e /dev/idw ]]; then
  echo "SKIP: /dev/idw is absent (build and load idw_core.ko to run this test)."
  exit 0
fi
if [[ ! -r /dev/idw || ! -w /dev/idw ]]; then
  echo "SKIP: /dev/idw is not accessible to this user (run with appropriate permissions)."
  exit 0
fi
"$ROOT/build/idwctl" resource-test 4 65536
