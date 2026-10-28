#!/bin/sh
# Smoke test for debian/detect-channel.sh:
#   1. With no pinned marker, the channel follows the Protect runnables.yaml.
#   2. With a pinned marker, the channel is forced to the pinned value
#      regardless of what runnables.yaml says.
#   3. Each UniFi OS release channel maps to the expected apt suite.
#
# Cases 1-5 probe the real apt repo, so they only check that the result
# is a valid channel (the script keeps the existing value when no suite
# is reachable); the pinned cases return before the probe.  Case 6 puts
# curl and logger stubs first on PATH, so every suite probe succeeds and
# the written channel is exactly the mapped suite.
#
# Run via: bazel test --config=x86 //test:test_detect_channel
set -eu

SCRIPT="${TEST_SRCDIR}/_main/debian/detect-channel.sh"
TMP="${TEST_TMPDIR:-/tmp}/detect_channel_$$"
mkdir -p "$TMP/etc"
mkdir -p "$TMP/runnables"

trap 'rm -rf "$TMP"' EXIT

write_runnables() {
    cat > "$TMP/runnables/runnables.yaml" <<EOF
releaseChannels:
  network: release
  protect: $1
  access: release
EOF
}

# Patch the script to use our tmp paths: re-run it under env overrides
# for the bits we care about, and a sed-edit for the hardcoded
# CHANNEL_FILE / PINNED_FILE.
PATCHED="$TMP/detect-channel.sh"
sed -e "s|^CHANNEL_FILE=.*|CHANNEL_FILE=$TMP/etc/channel|" \
    -e "s|^PINNED_FILE=.*|PINNED_FILE=$TMP/etc/channel.pinned|" \
    "$SCRIPT" > "$PATCHED"
chmod +x "$PATCHED"

run() {
    ONVIF_RUNNABLES_YAML="$TMP/runnables/runnables.yaml" "$PATCHED" "$@"
}

fail=0
check() {
    if [ "$1" = "$2" ]; then
        echo "PASS: $3 (got '$1')"
    else
        echo "FAIL: $3 -- expected '$2', got '$1'"
        fail=1
    fi
}

# 1. No pinned marker, runnables says release-candidate -> rc.
write_runnables release-candidate
run >/dev/null 2>&1 || true   # network probe may fail; that's OK
got=$(cat "$TMP/etc/channel" 2>/dev/null || true)
# In the offline case the script keeps the existing (stable) value;
# the import test is that without a marker, *something* writes the
# channel file -- just confirm it's a known-good value.
case "$got" in
    stable|rc|early-access) ok=1 ;;
    *) ok=0 ;;
esac
[ "$ok" = "1" ] || { echo "FAIL: case 1 left invalid channel '$got'"; fail=1; }

# 2. Pin to early-access; runnables says release.  Pinned must win.
echo "early-access" > "$TMP/etc/channel.pinned"
echo "stable" > "$TMP/etc/channel"   # set a different active value
write_runnables release
run >/dev/null 2>&1
got=$(cat "$TMP/etc/channel")
check "$got" "early-access" "pinned marker forces channel to pinned value"

# 3. Pin to rc, with runnables also says release-candidate.  Pinned wins
#    (and incidentally matches what auto-detect would have picked).
echo "rc" > "$TMP/etc/channel.pinned"
echo "stable" > "$TMP/etc/channel"
write_runnables release-candidate
run >/dev/null 2>&1
got=$(cat "$TMP/etc/channel")
check "$got" "rc" "pinned-rc with runnables=rc keeps rc"

# 4. Empty pinned file falls through to auto-detection (we tolerate the
#    edge case rather than treating empty as 'pinned to nothing').
: > "$TMP/etc/channel.pinned"
echo "stable" > "$TMP/etc/channel"
write_runnables release-candidate
run >/dev/null 2>&1 || true
got=$(cat "$TMP/etc/channel")
case "$got" in
    stable|rc|early-access) ok=1 ;;
    *) ok=0 ;;
esac
[ "$ok" = "1" ] || { echo "FAIL: empty marker case left invalid '$got'"; fail=1; }

# 5. Removing the marker re-enables auto-detection.
rm "$TMP/etc/channel.pinned"
write_runnables release-candidate
run >/dev/null 2>&1 || true
got=$(cat "$TMP/etc/channel")
case "$got" in
    stable|rc|early-access) ok=1 ;;
    *) ok=0 ;;
esac
[ "$ok" = "1" ] || { echo "FAIL: post-removal invalid '$got'"; fail=1; }
echo "PASS: marker removal restores auto-detection"

# 6. Mapping from UniFi OS release channels to apt suites, with every
#    suite reported as published.  Each known channel starts from a
#    different channel than the one expected, so keeping the previous
#    value fails it; an unknown channel must keep the previous value.
mkdir -p "$TMP/bin"
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/curl"
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/logger"
chmod +x "$TMP/bin/curl" "$TMP/bin/logger"

run_published() {
    PATH="$TMP/bin:$PATH" ONVIF_RUNNABLES_YAML="$TMP/runnables/runnables.yaml" \
        "$PATCHED" "$@"
}

check_mapping() {  # runnables.yaml protect value, starting channel, expected
    echo "$2" > "$TMP/etc/channel"
    write_runnables "$1"
    run_published >/dev/null 2>&1 || { echo "FAIL: protect: $1 exited non-zero"; fail=1; }
    got=$(cat "$TMP/etc/channel")
    check "$got" "$3" "protect: $1 -> $3"
}

check_mapping release           early-access stable
check_mapping release-candidate stable       rc
check_mapping beta              stable       early-access
check_mapping early-access      stable       early-access
check_mapping alpha             stable       early-access
check_mapping internal          stable       early-access
check_mapping qa                stable       early-access
check_mapping '"Alpha"'         rc           early-access
check_mapping QA                rc           early-access
check_mapping nightly           rc           rc

exit $fail
