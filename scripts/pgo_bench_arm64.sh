#!/bin/bash
set -e
cd "$BUILD_WORKSPACE_DIRECTORY"

BAZEL=~/.local/bin/bazel
PGO_EVENTS=${1:-50000}
BENCH_JSONL=test/testdata/bench_onvif.jsonl
PROFDATA=$(pwd)/pgo.profdata
arm64_sysroot=$($BAZEL info output_base 2>/dev/null)/external/arm64_sysroot/sysroot

test -f "$PROFDATA" || \
    (echo "Run 'bazel run //:pgo_bench_x86' first to collect the profile." && exit 1)
command -v qemu-aarch64-static >/dev/null || \
    (echo "Install QEMU: sudo apt-get install qemu-user-static" && exit 1)

echo "=== [1/4] Build baseline ARM64 binary ==="
$BAZEL build --config=arm64 //test:bench_onvif_listener
echo "=== [2/4] Baseline ARM64 benchmark under QEMU ==="
qemu-aarch64-static -L "$arm64_sysroot" \
    bazel-bin/test/bench_onvif_listener \
    "$BENCH_JSONL" "$PGO_EVENTS" 2>/dev/null
echo
echo "=== [3/4] Build ARM64 with cross-PGO + LTO ==="
echo "    (x86 profile applied to ARM64 build -- same LLVM IR structure)"
$BAZEL build --config=arm64 --config=lto \
    --copt=-fprofile-instr-use="$PROFDATA" \
    --linkopt=-fprofile-instr-use="$PROFDATA" \
    //test:bench_onvif_listener
echo "=== [4/4] ARM64 PGO + LTO benchmark under QEMU ==="
qemu-aarch64-static -L "$arm64_sysroot" \
    bazel-bin/test/bench_onvif_listener \
    "$BENCH_JSONL" "$PGO_EVENTS" 2>/dev/null
