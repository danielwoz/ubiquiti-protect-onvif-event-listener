#!/bin/sh
# install-apt-source.sh — (re)writes /etc/apt/sources.list.d/onvif-recorder.list
# based on the contents of /etc/onvif-recorder/channel.
set -e

CHANNEL_FILE=/etc/onvif-recorder/channel
SRC_FILE=/etc/apt/sources.list.d/onvif-recorder.list
KEYRING=/usr/share/keyrings/onvif-recorder-archive-keyring.gpg
REPO_URL=${ONVIF_REPO_URL:-https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener}

# Never write a source line whose signing key is absent: apt fails the whole
# `apt-get update` with NO_PUBKEY when a `signed-by=` keyring is missing, which
# breaks every other repo on the box too (and blocks `uos runnable install` on
# UniFi hardware).  The package ships the keyring, so this should not happen —
# but a UniFi OS update replaces /usr wholesale, and /etc/apt survives it, so
# the dangling combination is reachable.  Clear the stale source instead.
if [ ! -f "$KEYRING" ]; then
    if [ -f "$SRC_FILE" ]; then
        rm -f "$SRC_FILE"
        logger -t onvif-recorder-apt \
            "keyring $KEYRING missing; removed $SRC_FILE to keep apt working"
    fi
    exit 0
fi

CHANNEL=$(cat "$CHANNEL_FILE" 2>/dev/null | tr -d '[:space:]')
[ -n "$CHANNEL" ] || CHANNEL=stable

case "$CHANNEL" in
    stable|rc|early-access) ;;
    *)
        logger -t onvif-recorder-apt "unknown channel '$CHANNEL', using stable"
        CHANNEL=stable
        ;;
esac

DEB_ARCH=$(dpkg --print-architecture)

NEW="deb [arch=$DEB_ARCH signed-by=$KEYRING] $REPO_URL $CHANNEL main
"

if [ "$(cat "$SRC_FILE" 2>/dev/null)" != "$NEW" ]; then
    printf '%s' "$NEW" > "$SRC_FILE"
    logger -t onvif-recorder-apt "wrote $SRC_FILE ($CHANNEL)"
fi

exit 0
