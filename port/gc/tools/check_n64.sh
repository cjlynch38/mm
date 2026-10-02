#!/usr/bin/env bash
# Guard: the GameCube port must never change the N64 build.
# Rebuilds the N64 ROM with the normal Makefile and checks it still matches.
# Run from the repo root before every push.
set -euo pipefail
cd "$(dirname "$0")/../../.."

jobs=$(nproc)
log=$(mktemp)
trap 'rm -f "$log"' EXIT

echo "check_n64: building N64 ROM (make -j$jobs)..."
if ! make -j"$jobs" >"$log" 2>&1; then
    tail -30 "$log"
    echo "check_n64: FAILED (N64 build error)"
    exit 1
fi

if grep -q ': OK$' "$log" && ! grep -q 'FAILED' "$log"; then
    grep ': OK$' "$log"
    echo "check_n64: PASSED (N64 ROM still matches)"
else
    tail -30 "$log"
    echo "check_n64: FAILED (ROM checksum mismatch)"
    exit 1
fi
