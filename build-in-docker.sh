#!/usr/bin/env bash
# build-in-docker.sh — build onvif_recorder inside a Docker container
#
# Usage:
#   ./build-in-docker.sh              # build x86-64 and ARM64 binaries
#   ./build-in-docker.sh --x86        # x86-64 only
#   ./build-in-docker.sh --arm64      # ARM64 only
#   ./build-in-docker.sh --test       # build x86-64 + run tests
#   ./build-in-docker.sh --rebuild    # force rebuild of the Docker image
#
# Output binaries are written to dist/ in the project root:
#   dist/onvif_recorder          x86-64 binary
#   dist/onvif_recorder.arm64    ARM64 binary (for Dream Machine / NVR)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="onvif-recorder-builder"
CACHE_VOL="onvif-recorder-bazel-cache"

BUILD_X86=true
BUILD_ARM64=true
RUN_TESTS=false
REBUILD_IMAGE=false

for arg in "$@"; do
    case "$arg" in
        --x86)     BUILD_ARM64=false ;;
        --arm64)   BUILD_X86=false; BUILD_ARM64=true ;;
        --test)    BUILD_X86=true; BUILD_ARM64=false; RUN_TESTS=true ;;
        --rebuild) REBUILD_IMAGE=true ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# 1. Build the Docker image (once, or when --rebuild is passed)
# ---------------------------------------------------------------------------
if $REBUILD_IMAGE || ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "==> Building Docker image $IMAGE ..."
    docker build -t "$IMAGE" - <<'DOCKERFILE'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    cmake \
    ninja-build \
    perl \
    lld \
    nasm \
    python3-pip \
    curl \
    git \
  && pip3 install --break-system-packages cpplint \
  && rm -rf /var/lib/apt/lists/*

# Install Bazelisk as /usr/local/bin/bazel; it downloads the correct Bazel
# version from .bazelversion on first use.
RUN curl -fsSL -o /usr/local/bin/bazel \
    https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  && chmod +x /usr/local/bin/bazel
DOCKERFILE
fi

# ---------------------------------------------------------------------------
# 2. Ensure the Bazel cache volume exists (persists across runs)
# ---------------------------------------------------------------------------
docker volume create "$CACHE_VOL" > /dev/null

mkdir -p "$SCRIPT_DIR/dist"

# Run a single docker container for the build steps
run() {
    docker run --rm \
        -v "$SCRIPT_DIR:/build" \
        -v "$CACHE_VOL:/root/.cache/bazel" \
        -w /build \
        "$IMAGE" \
        bash -c "$1"
}

# ---------------------------------------------------------------------------
# 3. Build and copy outputs to dist/
# ---------------------------------------------------------------------------
if $BUILD_X86; then
    echo "==> Building x86-64 binary ..."
    run "bazel build //:onvif_recorder \
      && cp -f bazel-bin/onvif_recorder dist/onvif_recorder"
    echo "==> dist/onvif_recorder"
fi

if $RUN_TESTS; then
    echo "==> Running tests ..."
    run "bazel test //test:all"
fi

if $BUILD_ARM64; then
    echo "==> Building ARM64 binary ..."
    run "bazel build --config=arm64 //:onvif_recorder \
      && cp -f bazel-bin/onvif_recorder dist/onvif_recorder.arm64"
    echo "==> dist/onvif_recorder.arm64"
fi
