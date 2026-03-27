#!/bin/bash
set -e
cd "$BUILD_WORKSPACE_DIRECTORY"

BAZEL=~/.local/bin/bazel
PGO_EVENTS=${1:-50000}
BENCH_JSONL=test/testdata/bench_onvif.jsonl
PROFRAW=$(pwd)/pgo.profraw
PROFDATA=$(pwd)/pgo.profdata

echo "=== [1/6] Baseline: clang -O2, no PGO, no LTO ==="
$BAZEL run --config=clang //test:bench_onvif_listener \
    -- "$PGO_EVENTS" 2>/dev/null
echo
echo "=== [2/6] Build instrumented binary ==="
$BAZEL build --config=clang --config=pgo_instrument \
    //test:bench_onvif_listener
echo "=== [3/6] Collect profile ($PGO_EVENTS events) ==="
LLVM_PROFILE_FILE="$PROFRAW" \
    bazel-bin/test/bench_onvif_listener \
    "$BENCH_JSONL" "$PGO_EVENTS" 2>/dev/null
echo "=== [4/6] Merge profile ==="
# llvm-profdata-18 on Ubuntu 24.04; llvm-profdata-14 on Ubuntu 22.04.
LLVM_PROFDATA=$(command -v llvm-profdata-18 || command -v llvm-profdata-14 || echo llvm-profdata)
"$LLVM_PROFDATA" merge -output="$PROFDATA" "$PROFRAW"
echo "=== [5/6] Build PGO + LTO optimised binary ==="
$BAZEL build --config=clang --config=lto \
    --copt=-fprofile-instr-use="$PROFDATA" \
    --linkopt=-fprofile-instr-use="$PROFDATA" \
    //test:bench_onvif_listener
echo "=== [6/6] PGO + LTO benchmark ==="
$BAZEL run --config=clang --config=lto \
    --copt=-fprofile-instr-use="$PROFDATA" \
    --linkopt=-fprofile-instr-use="$PROFDATA" \
    //test:bench_onvif_listener -- "$PGO_EVENTS" 2>/dev/null
