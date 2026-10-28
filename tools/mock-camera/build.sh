#!/usr/bin/env bash
# Builds the MockCam image and pushes it to the homelab registry as :dev and :<gitrev>.
#
#   tools/mock-camera/build.sh             -> 127.0.0.1:30500/onvif-mockcam:{dev,<gitrev>}
#   tools/mock-camera/build.sh --no-push   -> build only (local test with docker run)
#
# The build context is staged in a temporary directory: this directory plus the two
# public-domain test images from test/testdata, which become the person and vehicle
# subjects.
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
REGISTRY=${REGISTRY:-127.0.0.1:30500}
IMAGE=onvif-mockcam
PUSH=1
[ "${1:-}" = "--no-push" ] && PUSH=0

GITREV=$(git -C "$REPO" rev-parse --short HEAD)
DIRTY=""
# Untracked files count too: everything under mockcam/ and subjects/ goes into the image.
[ -z "$(git -C "$REPO" status --porcelain -- tools/mock-camera test/testdata)" ] \
  || DIRTY="-dirty"
REV="${GITREV}${DIRTY}"

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
cp -r "$HERE/Dockerfile" "$HERE/make-subjects.sh" "$HERE/mockcam" "$HERE/subjects" "$CTX/"
find "$CTX/mockcam" -name __pycache__ -prune -exec rm -rf {} +
cp "$REPO/test/testdata/security_cam_person.jpg" "$CTX/subjects/person.jpg"
cp "$REPO/test/testdata/security_cam_vehicle.jpg" "$CTX/subjects/vehicle.jpg"

echo "building ${IMAGE} (git ${REV})"
docker build \
  --build-arg "MOCKCAM_FIRMWARE=${REV}" \
  --label "org.opencontainers.image.revision=${REV}" \
  --label "org.opencontainers.image.source=tools/mock-camera" \
  --label "build.date=$(date -Is)" \
  -t "${REGISTRY}/${IMAGE}:dev" \
  -t "${REGISTRY}/${IMAGE}:${REV}" \
  "$CTX"

if [ "$PUSH" = 1 ]; then
  docker push "${REGISTRY}/${IMAGE}:dev"
  docker push "${REGISTRY}/${IMAGE}:${REV}"
fi
[ -n "$DIRTY" ] && echo "WARNING: tools/mock-camera has uncommitted changes; tagged ${REV}" >&2
echo "image: ${REGISTRY}/${IMAGE}:${REV}"
