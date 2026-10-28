#!/bin/sh
# detect-channel.sh — detect the user's Protect release channel from
# UniFi OS's runnables.yaml and write a normalised name to
# /etc/onvif-recorder/channel.
#
# runnables.yaml lives at /data/unifi-core/config/runnables.yaml and looks
# like:
#
#   releaseChannels:
#     network: beta
#     protect: beta
#     access: release
#     ...
#
# UniFi OS values map to our apt suites: release -> stable,
# release-candidate -> rc, and the pre-release channels beta (Early
# Access; also accepted as early-access), alpha, internal and qa ->
# early-access.  If the file is missing or unparseable, or names any
# other channel, we leave the existing /etc/onvif-recorder/channel alone
# (or default to stable on first run).  Keep this mapping identical to
# BuildDetectChannelScript() in
# windows-installer/OnvifRecorderInstaller/Services/InstallScripts.cs.
#
# We also probe the apt repo: if the preferred suite has no Release file
# published yet (happens while a release is still only in early-access),
# drop through to the next best available suite instead of writing a
# dead channel that would 404 on apt-get update.
set -e

REPO_URL="${ONVIF_REPO_URL:-https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener}"
CHANNEL_FILE=/etc/onvif-recorder/channel
PINNED_FILE=/etc/onvif-recorder/channel.pinned
RUNNABLES=${ONVIF_RUNNABLES_YAML:-/data/unifi-core/config/runnables.yaml}

mkdir -p "$(dirname "$CHANNEL_FILE")"
if [ ! -f "$CHANNEL_FILE" ]; then
    echo "stable" > "$CHANNEL_FILE"
fi

# If the user has explicitly chosen a channel via the admin UI or the
# Windows installer, a sticky marker is written.  Respect it: skip
# auto-detection entirely.  (The marker is removed on `apt-get purge`, or
# by the Windows installer when Protect's own channel is chosen there in
# place of a different pinned one; either opts back into auto-detection.)
if [ -f "$PINNED_FILE" ]; then
    PINNED=$(tr -d '[:space:]' < "$PINNED_FILE")
    if [ -n "$PINNED" ]; then
        # Reconcile: ensure the active channel matches the pinned value.
        if [ "$(cat "$CHANNEL_FILE" 2>/dev/null)" != "$PINNED" ]; then
            echo "$PINNED" > "$CHANNEL_FILE"
        fi
        logger -t onvif-recorder-channel \
            "channel pinned by user to $PINNED -- skipping auto-detection"
        exit 0
    fi
fi

# Return 0 iff $REPO_URL/dists/$1/Release exists (HTTP 200).
suite_exists() {
    curl -fsI -o /dev/null "$REPO_URL/dists/$1/Release" 2>/dev/null
}

# Pick the first suite in the priority list that has a published Release.
# Echoes the suite name on stdout, or nothing on total failure.
select_available_suite() {
    for candidate in "$@"; do
        if suite_exists "$candidate"; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

[ -r "$RUNNABLES" ] || exit 0

# Extract `protect:` under the `releaseChannels:` block.  Tolerant of
# whitespace + optional quotes; awk keeps this dependency-free.
RAW=$(awk '
    /^releaseChannels:/ { in_block=1; next }
    in_block && /^[^[:space:]]/ { in_block=0 }
    in_block && /^[[:space:]]+protect[[:space:]]*:/ {
        sub(/^[^:]*:[[:space:]]*/, "")
        gsub(/["'"'"']/, "")
        sub(/[[:space:]]*#.*$/, "")
        print; exit
    }
' "$RUNNABLES" 2>/dev/null | tr -d '[:space:]' | tr '[:upper:]' '[:lower:]')

case "$RAW" in
    release)           PREFERRED=stable ;;
    release-candidate) PREFERRED=rc ;;
    beta|early-access|alpha|internal|qa) PREFERRED=early-access ;;
    *) exit 0 ;;  # unknown / empty — keep previous value
esac

# Prefer the detected channel, but fall back through stable -> rc ->
# early-access if the preferred suite isn't reachable.
NEW=$(select_available_suite "$PREFERRED" stable rc early-access) || NEW=""

if [ -z "$NEW" ]; then
    # Couldn't reach any suite (network down, rare).  Leave the existing
    # channel file alone so we don't replace a working value with an
    # unreachable one.
    logger -t onvif-recorder-channel \
        "no apt suites reachable at $REPO_URL; keeping existing channel"
    exit 0
fi

if [ "$(cat "$CHANNEL_FILE" 2>/dev/null)" != "$NEW" ]; then
    echo "$NEW" > "$CHANNEL_FILE"
    if [ "$NEW" != "$PREFERRED" ]; then
        logger -t onvif-recorder-channel \
            "Protect channel detected as $PREFERRED (runnables.yaml=$RAW); using $NEW as fallback"
    else
        logger -t onvif-recorder-channel \
            "Protect channel detected as $NEW (runnables.yaml=$RAW)"
    fi
fi

exit 0
