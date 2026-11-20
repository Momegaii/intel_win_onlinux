#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/build/idwctl" info
"$ROOT/build/idwctl" selftest
