#!/bin/sh
# Checks for the Debian maintainer scripts:
#   1. prerm, postinst, postrm and preinst pass `sh -n`.
#   2. prerm's upgrade) branch does not stop onvif-recorder.service, so the
#      running recorder keeps serving while dpkg unpacks the new files.
#   3. prerm's remove|deconfigure) branch still stops the service, and on
#      remove runs `onvif-recorder --rollback=all` after the stop.
#   4. postinst's configure upgrade arm restarts the service with
#      `systemctl try-restart` after `systemctl daemon-reload`.
#
# prerm is also run with stub systemctl / systemd-cat / onvif-recorder
# commands that record their arguments, so the checks cover what the script
# does as well as what it says.  postinst touches fixed system paths, so it
# is only checked as text.
#
# Run via: scripts/bz test --config=x86 //test:test_debian_scripts
set -eu

DEB="${TEST_SRCDIR}/_main/debian"
PRERM="$DEB/prerm"
POSTINST="$DEB/postinst"
POSTRM="$DEB/postrm"
PREINST="$DEB/preinst"
TMP="${TEST_TMPDIR:-/tmp}/debian_scripts_$$"
mkdir -p "$TMP/bin"

trap 'rm -rf "$TMP"' EXIT

fail=0
pass() { echo "PASS: $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

# Prints the body of case label "$2)" in file "$1" up to its `;;`, with
# comments removed.  Exits 2 when the label is missing.
branch() {
    awk -v want="$2)" '
        { t = $0; gsub(/^[ \t]+|[ \t]+$/, "", t) }
        !inb && t == want { inb = 1; next }
        inb && t == ";;" { exit }
        inb { sub(/#.*/, ""); print }
        END { if (!inb) exit 2 }' "$1"
}

# Reads a configure) branch on stdin and prints the else-arm of its
# `if [ -z "$2" ]` block (the upgrade path).  Exits 2 when the block is
# missing.
upgrade_arm() {
    awk '
        { t = $0; gsub(/^[ \t]+|[ \t]+$/, "", t) }
        depth == 0 && t ~ /^if \[ -z "\$2" \]; then$/ {
            depth = 1; found = 1; next
        }
        depth == 0 { next }
        t ~ /^if[ \t]/ { depth++ }
        t ~ /^fi([ \t;]|$)/ { depth--; if (depth == 0) exit }
        depth == 1 && t == "else" { arm = 1; next }
        arm { print t }
        END { if (!found) exit 2 }'
}

# Prints the line number of the first line in "$1" containing "$2", or
# nothing.
line_of() {
    grep -nF -- "$2" "$1" | head -n 1 | cut -d: -f1
}

# ----- 1. Syntax -----
for f in "$PRERM" "$POSTINST" "$POSTRM" "$PREINST"; do
    name=$(basename "$f")
    if [ ! -f "$f" ]; then
        bad "$name is missing from runfiles"
    elif err=$(sh -n "$f" 2>&1); then
        pass "sh -n $name"
    else
        bad "sh -n $name: $err"
    fi
done

# ----- 2. prerm upgrade) text -----
if body=$(branch "$PRERM" upgrade); then
    if printf '%s\n' "$body" | grep -q 'systemctl.*stop'; then
        bad "prerm upgrade) stops a unit"
    else
        pass "prerm upgrade) has no systemctl stop"
    fi
else
    bad "prerm has no upgrade) branch"
fi

# ----- 3. prerm remove|deconfigure) text -----
if body=$(branch "$PRERM" 'remove|deconfigure'); then
    if printf '%s\n' "$body" | grep -q 'systemctl stop onvif-recorder\.service'; then
        pass "prerm remove|deconfigure) stops the service"
    else
        bad "prerm remove|deconfigure) does not stop the service"
    fi
    if printf '%s\n' "$body" | grep -q -- '--rollback=all'; then
        pass "prerm remove|deconfigure) runs --rollback=all"
    else
        bad "prerm remove|deconfigure) does not run --rollback=all"
    fi
else
    bad "prerm has no remove|deconfigure) branch"
fi

# ----- prerm run with stub commands -----
LOG="$TMP/calls.log"
cat > "$TMP/bin/systemctl" <<EOF
#!/bin/sh
echo "systemctl \$*" >> "$LOG"
EOF
cat > "$TMP/bin/systemd-cat" <<'EOF'
#!/bin/sh
cat > /dev/null
EOF
cat > "$TMP/bin/onvif-recorder" <<EOF
#!/bin/sh
echo "onvif-recorder \$*" >> "$LOG"
EOF
chmod +x "$TMP/bin/systemctl" "$TMP/bin/systemd-cat" "$TMP/bin/onvif-recorder"
sed "s|/usr/bin/onvif-recorder|$TMP/bin/onvif-recorder|g" "$PRERM" > "$TMP/prerm"

# Runs the stubbed prerm with "$@"; the exit status is left in $rc.
run_prerm() {
    : > "$LOG"
    rc=0
    PATH="$TMP/bin:$PATH" sh "$TMP/prerm" "$@" > /dev/null 2>&1 || rc=$?
}

run_prerm upgrade 9.9.9
if [ "$rc" -ne 0 ]; then
    bad "prerm upgrade exited $rc"
elif grep -Eq ' stop| disable|^onvif-recorder' "$LOG"; then
    bad "prerm upgrade ran: $(tr '\n' ';' < "$LOG")"
else
    pass "prerm upgrade leaves the service and timers running"
fi

run_prerm remove
stop_at=$(line_of "$LOG" "systemctl stop onvif-recorder.service")
rollback_at=$(line_of "$LOG" "onvif-recorder --rollback=all")
if [ "$rc" -ne 0 ]; then
    bad "prerm remove exited $rc"
elif [ -z "$stop_at" ]; then
    bad "prerm remove did not stop the service"
elif [ -z "$rollback_at" ]; then
    bad "prerm remove did not run --rollback=all"
elif [ "$stop_at" -gt "$rollback_at" ]; then
    bad "prerm remove ran --rollback=all before stopping the service"
else
    pass "prerm remove stops the service, then rolls back"
fi
for timer in onvif-recorder-channel.timer onvif-recorder-autoupdate.timer; do
    if grep -qF "systemctl disable --now $timer" "$LOG"; then
        pass "prerm remove disables $timer"
    else
        bad "prerm remove did not disable $timer"
    fi
done

run_prerm deconfigure
if [ "$rc" -ne 0 ]; then
    bad "prerm deconfigure exited $rc"
elif ! grep -qF "systemctl stop onvif-recorder.service" "$LOG"; then
    bad "prerm deconfigure did not stop the service"
elif grep -q '^onvif-recorder' "$LOG"; then
    bad "prerm deconfigure ran the rollback"
else
    pass "prerm deconfigure stops the service without rolling back"
fi

run_prerm failed-upgrade 9.9.9
if [ "$rc" -ne 0 ] || [ -s "$LOG" ]; then
    bad "prerm failed-upgrade exited $rc and ran: $(tr '\n' ';' < "$LOG")"
else
    pass "prerm failed-upgrade does nothing"
fi

# ----- 4. postinst upgrade path -----
if configure=$(branch "$POSTINST" configure); then
    printf '%s\n' "$configure" > "$TMP/configure"
    if arm=$(upgrade_arm < "$TMP/configure"); then
        if printf '%s\n' "$arm" | \
                grep -q '^systemctl try-restart onvif-recorder\.service'; then
            pass "postinst upgrade path try-restarts the service"
        else
            bad "postinst upgrade path does not try-restart the service"
        fi
    else
        bad "postinst configure) has no if [ -z \"\$2\" ] block"
    fi
    reload_at=$(line_of "$TMP/configure" "systemctl daemon-reload")
    restart_at=$(line_of "$TMP/configure" "systemctl try-restart onvif-recorder.service")
    if [ -n "$reload_at" ] && [ -n "$restart_at" ] && \
            [ "$reload_at" -lt "$restart_at" ]; then
        pass "postinst reloads systemd before the restart"
    else
        bad "postinst does not run daemon-reload before try-restart"
    fi
    if grep -q 'systemctl stop onvif-recorder' "$TMP/configure"; then
        bad "postinst configure) stops the service"
    else
        pass "postinst configure) does not stop the service"
    fi
else
    bad "postinst has no configure) branch"
fi

exit $fail
