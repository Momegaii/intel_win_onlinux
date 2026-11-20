#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/idwctl"

expect_usage_error() {
    local status=0
    "$BIN" "$@" >/dev/null 2>&1 || status=$?
    if [[ $status -ne 1 ]]; then
        echo "FAIL: expected usage error (exit 1), got $status for: $*" >&2
        return 1
    fi
}

expect_usage_error memory-test 4294967297
expect_usage_error memory-test 4trailing
expect_usage_error memory-test 1 -1
expect_usage_error resource-test 2 12trailing
expect_usage_error gpu-submit 4294967297
expect_usage_error gpu-submit 0
expect_usage_error gpu-submit 1 extra
expect_usage_error gem-resource-test extra
expect_usage_error info --unknown

echo "PASS: CLI rejects overflowing, partial, signed, and unexpected arguments before device access."
