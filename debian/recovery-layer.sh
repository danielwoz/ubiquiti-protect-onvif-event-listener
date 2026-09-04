#!/bin/sh
# Deploy the persistent recovery layer to /data.
#
# UDM/UNVR firmware upgrades replace the squashfs lower layer of the overlay
# root, wiping /usr and /lib (the binary, the systemd unit) and parts of /etc
# (notably /etc/apt/sources.list.d).  They preserve /data and /etc/cron.d.
# This script writes a small self-contained recovery layer into those two
# surviving locations so the next boot can put the package back.
#
# Run from two places:
#   - debian/postinst, on every install/upgrade.  This is what gets the layer
#     onto machines installed straight from apt, which previously had no
#     recovery at all -- only the curl bootstrap deployed it (issue #51).
#   - gh-pages/install.sh, which delegates here when the packaged copy exists.
#
# The files it writes are deliberately NOT dpkg-owned: `apt purge` removes
# everything dpkg knows about, and the whole point is to outlive that.  See
# CLAUDE.md "Uninstall — full purge" to remove them by hand.
set -e

REPO_URL="${REPO_URL:-https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener}"

# Only meaningful on UniFi hardware, where /data is the persistent volume.
if [ ! -d /data ]; then
    echo "no /data -- skipping recovery layer (not UniFi hardware)"
    exit 0
fi

# 5. Persistent recovery layer (survives firmware upgrades + apt purge).
#
# UDM firmware upgrades wipe /usr/bin/onvif-recorder, the systemd unit, and
# /etc/apt/sources.list.d/ but preserve /data/ and /etc/cron.d/.  We install
# a small recovery layer on /data/onvif-recorder/ plus an @reboot trigger
# in /etc/cron.d/ so that on the next boot after a firmware upgrade we can:
#   (a) restore the config from the backup tarball in /data/, and
#   (b) re-run this install script to bring the package back.
#
# These files are intentionally NOT in the .deb package: they must survive
# `apt purge`, because purge wipes /etc/onvif-recorder/ and the apt source
# along with everything else dpkg knows about.  To remove the recovery
# layer too, see CLAUDE.md "Uninstall — full purge".
DATA_DIR=/data/onvif-recorder
echo "==> Installing recovery layer to $DATA_DIR ..."
mkdir -p "$DATA_DIR/backups"

# Stage install.sh so boot-restore can call it after a firmware wipe.  The
# package ships a copy; fall back to the network only if that is missing.
if [ -f /usr/share/onvif-recorder/install.sh ]; then
    cp /usr/share/onvif-recorder/install.sh "$DATA_DIR/install.sh"
elif [ -f "$0" ] && [ -r "$0" ] && [ "$(basename "$0")" = "install.sh" ]; then
    cp "$0" "$DATA_DIR/install.sh"
else
    curl -fsSL "$REPO_URL/install.sh" -o "$DATA_DIR/install.sh"
fi

cat > "$DATA_DIR/boot-restore.sh" <<'EOF_BOOT_RESTORE'
#!/bin/sh
# Auto-reinstall onvif-recorder after a firmware wipe.
# Triggered by /etc/cron.d/onvif-recorder-boot-restore @reboot.
exec >> /var/log/onvif-recorder-boot-restore.log 2>&1
echo "=== $(date -Is) boot-restore start ==="

# Wait for the system to settle (network, systemd, apt locks).
sleep 60

# Normal boot: package is already installed, nothing to do.
if dpkg -s onvif-recorder >/dev/null 2>&1; then
    echo "package already installed; nothing to do"
    exit 0
fi

# Restore the most recent config backup before reinstalling so the new
# package start-up sees the user's last channel / admin settings / cached
# protect-user-id.
BACKUP=/data/onvif-recorder/backups/config-current.tar.gz
if [ -f "$BACKUP" ]; then
    echo "restoring config from $BACKUP"
    tar -xzf "$BACKUP" -C /
fi

# Wipe-recovery branch: if the binary is gone entirely AND we have a
# config backup, this is a firmware-wipe scenario, not a routine boot.
# A user who opted out of auto-updates didn't ask for their service to
# disappear, so opting out doesn't apply here -- reinstall unconditionally.
if [ ! -x /usr/bin/onvif-recorder ] && \
   [ -f /data/onvif-recorder/backups/config-current.tar.gz ]; then
    echo "wipe-recovery: /usr/bin/onvif-recorder is missing; ignoring consent gate"
    sh /data/onvif-recorder/install.sh
    echo "=== $(date -Is) boot-restore done (wipe-recovery) ==="
    exit 0
fi

# Routine boot: apply the consent gate.  The file is regenerated from
# /etc/onvif-recorder/config.json on every install/upgrade so flipping
# autoupdate_enabled in the admin UI takes effect on the *next* boot.
consent=$(cat /data/onvif-recorder/.autoupdate-consent 2>/dev/null)
if [ "$consent" != "true" ]; then
    echo "auto-reinstall consent not granted (.autoupdate-consent != true)"
    echo "to enable: echo true > /data/onvif-recorder/.autoupdate-consent"
    exit 0
fi

echo "running /data/onvif-recorder/install.sh"
sh /data/onvif-recorder/install.sh
echo "=== $(date -Is) boot-restore done ==="
EOF_BOOT_RESTORE
chmod 0755 "$DATA_DIR/boot-restore.sh"

cat > "$DATA_DIR/backup.sh" <<'EOF_BACKUP'
#!/bin/sh
# Snapshot onvif-recorder configuration to /data so it can be restored
# after a firmware wipe / apt purge.  Run by:
#   - install.sh, once at first install / upgrade
#   - /etc/cron.d/onvif-recorder-backup, daily
set -e
BACKUP_DIR=/data/onvif-recorder/backups
mkdir -p "$BACKUP_DIR"

# Build a tarball atomically — write to .tmp, mv into place.
tmp="$BACKUP_DIR/.config-current.tar.gz.tmp"
paths="etc/onvif-recorder"
[ -f /etc/default/onvif-recorder ]       && paths="$paths etc/default/onvif-recorder"
[ -f /etc/default/onvif-recorder.local ] && paths="$paths etc/default/onvif-recorder.local"
[ -d /var/lib/onvif-recorder ]           && paths="$paths var/lib/onvif-recorder"
# shellcheck disable=SC2086
tar -czf "$tmp" -C / $paths 2>/dev/null
mv -f "$tmp" "$BACKUP_DIR/config-current.tar.gz"

# Keep one dated snapshot per week (Sundays), prune to the last 4.
if [ "$(date +%w)" = "0" ]; then
    cp "$BACKUP_DIR/config-current.tar.gz" \
       "$BACKUP_DIR/config-$(date +%Y-%m-%d).tar.gz"
fi
ls -1t "$BACKUP_DIR"/config-2[0-9]*.tar.gz 2>/dev/null \
    | tail -n +5 | xargs -r rm -f

size=$(stat -c %s "$BACKUP_DIR/config-current.tar.gz")
echo "$(date -Is) backup written: $BACKUP_DIR/config-current.tar.gz ($size bytes)"
EOF_BACKUP
chmod 0755 "$DATA_DIR/backup.sh"

# Take an immediate backup so /data is current right after install.
"$DATA_DIR/backup.sh"

# Cron entries — these live in /etc/cron.d/ (which survives firmware
# upgrades on UDM) and are intentionally outside the .deb so that
# `apt purge` does not remove them.
cat > /etc/cron.d/onvif-recorder-boot-restore <<'EOF_CRON_BOOT'
# ONVIF Recorder — auto-reinstall after firmware upgrade.
# NOT managed by dpkg; survives `apt purge`.
# To remove permanently, see CLAUDE.md "Uninstall — full purge".
SHELL=/bin/sh
PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
@reboot root /data/onvif-recorder/boot-restore.sh
EOF_CRON_BOOT
chmod 0644 /etc/cron.d/onvif-recorder-boot-restore

cat > /etc/cron.d/onvif-recorder-backup <<'EOF_CRON_BACKUP'
# ONVIF Recorder — daily config backup to /data.
# NOT managed by dpkg; survives `apt purge`.
SHELL=/bin/sh
PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
17 4 * * * root /data/onvif-recorder/backup.sh >>/var/log/onvif-recorder-backup.log 2>&1
EOF_CRON_BACKUP
chmod 0644 /etc/cron.d/onvif-recorder-backup

# Sync the auto-reinstall consent with the admin-server setting.
# Default is "true": running install.sh is itself an act of consent to
# auto-recover after a firmware wipe.  The only way to land "false" is an
# explicit `"autoupdate_enabled": false` in config.json (settable via the
# admin UI).  Missing field, malformed file, or absent python3 all keep
# the default-true behaviour.
if command -v python3 >/dev/null 2>&1 && [ -f /etc/onvif-recorder/config.json ]; then
    auto=$(python3 -c '
import json
try:
    d = json.load(open("/etc/onvif-recorder/config.json"))
    v = d.get("autoupdate_enabled")
    # Strings "true"/"false" round-trip via the admin form; booleans round-trip
    # via direct API.  Treat anything explicitly "false" / False as opt-out;
    # everything else (None, "", missing) as consent.
    if v is False or (isinstance(v, str) and v.lower() == "false"):
        print("false")
    else:
        print("true")
except Exception:
    print("true")
' 2>/dev/null || echo true)
else
    auto=true
fi
echo "${auto:-true}" > "$DATA_DIR/.autoupdate-consent"

echo "recovery layer ready at $DATA_DIR (boot auto-reinstall: ${auto:-true})"
