#!/bin/bash
# Benchmark x86 PGO + ThinLTO speedup.
# Stages: baseline → instrument → collect profile → PGO+LTO build → benchmark.
#
# Usage:
#   scripts/bz run --config=x86 //:pgo_bench_x86
#   scripts/bz run --config=x86 //:pgo_bench_x86 -- 100000   # custom event count
set -e
cd "$BUILD_WORKSPACE_DIRECTORY"

SCRIPT_DIR="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
BZ="$SCRIPT_DIR/bz"
PGO_EVENTS=${1:-50000}
BENCH_JSONL=test/testdata/bench_onvif.jsonl
PROFRAW=$(mktemp /tmp/pgo_x86_bench.XXXXXX.profraw)
PROFDATA=$(mktemp /tmp/pgo_x86_bench.XXXXXX.profdata)
trap 'rm -f "$PROFRAW" "$PROFDATA"' EXIT

echo "=== [1/6] Baseline: clang -O2, no PGO, no LTO ==="
"$BZ" run --config=x86 //test:bench_onvif_listener \
    -- "$PGO_EVENTS" 2>/dev/null
echo

echo "=== [2/6] Build instrumented binary ==="
"$BZ" build --config=x86 --config=pgo_instrument \
    //test:bench_onvif_listener

echo "=== [3/6] Collect profile ($PGO_EVENTS events) ==="
LLVM_PROFILE_FILE="$PROFRAW" \
    bazel-bin/test/bench_onvif_listener \
    "$BENCH_JSONL" "$PGO_EVENTS" 2>/dev/null

echo "=== [4/6] Merge profile ==="
LLVM_PROFDATA=$(command -v llvm-profdata-18 || command -v llvm-profdata-14 || echo llvm-profdata)
"$LLVM_PROFDATA" merge -output="$PROFDATA" "$PROFRAW"

echo "=== [5/6] Build PGO + LTO optimised binary ==="
"$BZ" build --config=x86 --config=lto \
    --copt=-fprofile-instr-use="$PROFDATA" \
    --linkopt=-fprofile-instr-use="$PROFDATA" \
    //test:bench_onvif_listener

echo "=== [6/6] PGO + LTO benchmark ==="
"$BZ" run --config=x86 --config=lto \
    --copt=-fprofile-instr-use="$PROFDATA" \
    --linkopt=-fprofile-instr-use="$PROFDATA" \
    //test:bench_onvif_listener -- "$PGO_EVENTS" 2>/dev/null
