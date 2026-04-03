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

# File that records the git commit SHA of the last successful ARM64 Docker build.
# Lets the pre-push hook (and manual invocations) skip a rebuild when nothing has
# changed since the previous run.  Listed in .gitignore.
ARM64_BUILD_CACHE="$SCRIPT_DIR/.docker_arm64_cache"

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

# Current HEAD commit SHA -- used to detect whether anything changed since the
# last successful ARM64 Docker build.
CURRENT_SHA=$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null || echo "")

# --rebuild clears the ARM64 build cache so the next run always rebuilds.
if $REBUILD_IMAGE; then
    rm -f "$ARM64_BUILD_CACHE"
fi

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
#    Sub-directories inside the volume provide per-config isolation:
#      /root/.cache/bazel/x86       — x86 build cache
#      /root/.cache/bazel/arm64     — ARM64 cross-compile cache
#      /root/.cache/bazel/arm64_release — ARM64 PGO+LTO release cache
# ---------------------------------------------------------------------------
docker volume create "$CACHE_VOL" > /dev/null

mkdir -p "$SCRIPT_DIR/dist"

# Run a Docker container for a single build step.
# $1 = Bazel config name (used as --output_base sub-directory)
# $2 = shell command to run inside the container
run() {
    local config="$1"
    local cmd="$2"
    docker run --rm \
        -v "$SCRIPT_DIR:/build" \
        -v "$CACHE_VOL:/root/.cache/bazel" \
        -w /build \
        "$IMAGE" \
        bash -c "bazel --output_base=/root/.cache/bazel/$config $cmd"
}

# ---------------------------------------------------------------------------
# 3. Build and copy outputs to dist/
# ---------------------------------------------------------------------------
if $BUILD_X86; then
    echo "==> Building x86-64 binary ..."
    run "x86" "build --config=x86 //:onvif_recorder \
      && cp -f bazel-bin/onvif_recorder dist/onvif_recorder"
    echo "==> dist/onvif_recorder"
fi

if $RUN_TESTS; then
    echo "==> Running tests ..."
    run "x86" "test --config=x86 //test:all"
fi

if $BUILD_ARM64; then
    CACHED_SHA=$(cat "$ARM64_BUILD_CACHE" 2>/dev/null || echo "")
    if [ -n "$CURRENT_SHA" ] && [ "$CURRENT_SHA" = "$CACHED_SHA" ]; then
        echo "==> ARM64 Docker build skipped (already built at $(git -C "$SCRIPT_DIR" log -1 --oneline 2>/dev/null || echo "$CURRENT_SHA"))"
    else
        echo "==> Building ARM64 binary ..."
        run "arm64" "build --config=arm64 //:onvif_recorder \
          && cp -f bazel-bin/onvif_recorder dist/onvif_recorder.arm64"
        echo "==> dist/onvif_recorder.arm64"
        echo "$CURRENT_SHA" > "$ARM64_BUILD_CACHE"
    fi
fi
