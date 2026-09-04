#!/bin/sh
# install.sh — one-line installer for onvif-recorder.
#
#   curl -fsSL https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/install.sh | sh
#
# Detects the user's UniFi Protect release channel (Stable / Release Candidate
# / Early Access) and configures APT to pull matching onvif-recorder builds.
# Safe to re-run; it just (re)writes the keyring + sources list.
set -e

REPO_URL="https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener"
KEYRING=/usr/share/keyrings/onvif-recorder-archive-keyring.gpg
SRC_FILE=/etc/apt/sources.list.d/onvif-recorder.list
CHANNEL_FILE=/etc/onvif-recorder/channel

if [ "$(id -u)" -ne 0 ]; then
    echo "This installer must run as root." >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get not found — this installer only supports Debian-based UniFi OS." >&2
    exit 1
fi

DEB_ARCH=$(dpkg --print-architecture)
case "$DEB_ARCH" in
    arm64) ;;
    *)
        echo "Unsupported architecture: $DEB_ARCH (only arm64 is published)." >&2
        exit 1
        ;;
esac

# Return 0 iff $REPO_URL/dists/$1/Release exists (HTTP 200).
suite_exists() {
    curl -fsI -o /dev/null "$REPO_URL/dists/$1/Release" 2>/dev/null
}

# Pick the first suite in the preference list that actually has a published
# Release file.  Falls back gracefully when a channel hasn't been cut yet
# (the common failure mode is `stable` missing because a release is still
# in `rc` or `early-access`).
select_available_suite() {
    for candidate in "$@"; do
        if suite_exists "$candidate"; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

# 1. GPG keyring.
echo "==> Fetching signing key ..."
mkdir -p /usr/share/keyrings
curl -fsSL "$REPO_URL/onvif-recorder.gpg" -o /tmp/onvif-recorder.gpg
gpg --dearmor < /tmp/onvif-recorder.gpg > "$KEYRING"
chmod 0644 "$KEYRING"
rm -f /tmp/onvif-recorder.gpg

# 2. Channel detection.
# Priority order for the default install:
#   preferred (existing channel file, if set and valid) -> stable -> rc -> early-access.
# If a suite's Release file isn't published yet, drop through to the next one
# rather than writing a dead channel that'd 404 on apt-get update.
mkdir -p /etc/onvif-recorder
PREFERRED=""
if [ -f "$CHANNEL_FILE" ]; then
    PREFERRED=$(tr -d '[:space:]' < "$CHANNEL_FILE")
fi
case "$PREFERRED" in
    stable|rc|early-access) ;;
    *) PREFERRED="" ;;
esac

if [ -n "$PREFERRED" ]; then
    CHANNEL=$(select_available_suite "$PREFERRED" stable rc early-access) || CHANNEL=""
else
    CHANNEL=$(select_available_suite stable rc early-access) || CHANNEL=""
fi

if [ -z "$CHANNEL" ]; then
    echo "No published apt suite reachable at $REPO_URL (stable/rc/early-access all 404)." >&2
    echo "Check your network or wait for the next release to be promoted." >&2
    exit 1
fi

if [ -n "$PREFERRED" ] && [ "$CHANNEL" != "$PREFERRED" ]; then
    echo "==> Preferred channel '$PREFERRED' not yet published; using '$CHANNEL'"
fi

echo "$CHANNEL" > "$CHANNEL_FILE"
echo "==> Using channel: $CHANNEL"

# 3. APT source.
echo "deb [arch=$DEB_ARCH signed-by=$KEYRING] $REPO_URL $CHANNEL main" \
    > "$SRC_FILE"

# 4. Install.
echo "==> apt-get update ..."
apt-get update \
    -o Dir::Etc::sourcelist="$SRC_FILE" \
    -o Dir::Etc::sourceparts=- \
    -o APT::Get::List-Cleanup=0

echo "==> Installing onvif-recorder ..."
apt-get install -y onvif-recorder

# 5. Persistent recovery layer (survives firmware upgrades + apt purge).
DATA_DIR=/data/onvif-recorder
#
# The package now owns this logic at /usr/share/onvif-recorder/recovery-layer.sh
# and runs it from postinst, so an apt-only install gets recovery too (issue
# #51).  We are past `apt-get install` by this point, so the packaged copy is
# present; delegate to it rather than keeping a second copy of the heredocs
# here and letting the two drift.
#
# The inline fallback covers upgrading FROM a version that predates the
# packaged script, where the freshly installed .deb may still be the old one.
if [ -x /usr/share/onvif-recorder/recovery-layer.sh ]; then
    echo "==> Installing recovery layer ..."
    /usr/share/onvif-recorder/recovery-layer.sh
else
    echo "==> Installing recovery layer (inline fallback) ..."
    mkdir -p "$DATA_DIR/backups"
    if [ -f "$0" ] && [ -r "$0" ]; then
        cp "$0" "$DATA_DIR/install.sh"
    else
        curl -fsSL "$REPO_URL/install.sh" -o "$DATA_DIR/install.sh"
    fi
    chmod 0755 "$DATA_DIR/install.sh"
    echo "note: older package -- boot-restore staged, cron entries will be"
    echo "      written on the next install once recovery-layer.sh ships."
fi

auto=$(cat "$DATA_DIR/.autoupdate-consent" 2>/dev/null || echo unknown)
echo "==> Recovery layer at $DATA_DIR (boot auto-reinstall: $auto)."
echo "    To opt in/out manually:"
echo "        echo true  > $DATA_DIR/.autoupdate-consent   # enable"
echo "        echo false > $DATA_DIR/.autoupdate-consent   # disable"
echo ""
echo "Done.  Manage it from https://<this-device>/onvif/admin/"
echo "Channel can be changed by editing $CHANNEL_FILE or via the admin UI."
