#!/bin/bash
# Collect a native ARM64 PGO profile under QEMU and stage it for commit.
# The resulting pgo_arm64.profdata is used by --config=arm64_release builds.
#
# Prerequisites:
#   sudo apt-get install qemu-user-static
#
# Usage:
#   bazel run //:pgo_collect_arm64
#   bazel run //:pgo_collect_arm64 -- 100000   # custom event count
set -e
cd "$BUILD_WORKSPACE_DIRECTORY"

SCRIPT_DIR="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
BZ="$SCRIPT_DIR/bz"
PGO_EVENTS=${1:-50000}
BENCH_JSONL=test/testdata/bench_onvif.jsonl
PROFRAW=$(pwd)/pgo_arm64.profraw
PROFDATA=$(pwd)/pgo_arm64.profdata
arm64_sysroot=$("$BZ" --output_base="$HOME/.cache/bazel/arm64" info output_base 2>/dev/null)/external/arm64_sysroot/sysroot/usr/aarch64-linux-gnu

command -v qemu-aarch64-static >/dev/null 2>&1 || {
    echo "qemu-aarch64-static not found."
    echo "Install with: sudo apt-get install qemu-user-static"
    exit 1
}

echo "=== [1/6] Build baseline ARM64 binary ==="
"$BZ" build --config=arm64 //test:bench_onvif_listener
echo "=== [2/6] Baseline ARM64 benchmark under QEMU ==="
qemu-aarch64-static -L "$arm64_sysroot" \
    bazel-bin/test/bench_onvif_listener \
    "$BENCH_JSONL" "$PGO_EVENTS" 2>/dev/null
echo

echo "=== [3/6] Build instrumented ARM64 binary ==="
"$BZ" build --config=arm64 --config=pgo_instrument \
    //test:bench_onvif_listener

echo "=== [4/6] Collect ARM64 profile ($PGO_EVENTS events) under QEMU ==="
LLVM_PROFILE_FILE="$PROFRAW" \
    qemu-aarch64-static -L "$arm64_sysroot" \
    bazel-bin/test/bench_onvif_listener \
    "$BENCH_JSONL" "$PGO_EVENTS" 2>/dev/null

echo "=== [5/6] Merge ARM64 profile ==="
LLVM_PROFDATA=$(command -v llvm-profdata-18 || command -v llvm-profdata-14 || echo llvm-profdata)
"$LLVM_PROFDATA" merge -output="$PROFDATA" "$PROFRAW"
rm -f "$PROFRAW"

echo "=== [6/6] Build ARM64 PGO + LTO binary and benchmark ==="
"$BZ" build --config=arm64_release //test:bench_onvif_listener
qemu-aarch64-static -L "$arm64_sysroot" \
    bazel-bin/test/bench_onvif_listener \
    "$BENCH_JSONL" "$PGO_EVENTS" 2>/dev/null
echo

echo "Profile saved: pgo_arm64.profdata (staged for commit)"
git add "$PROFDATA"
