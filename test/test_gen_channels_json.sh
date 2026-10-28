#!/bin/sh
# Checks for scripts/gen_channels_json.py against the apt-repo fixtures in
# test/testdata/apt (early-access and stable published, rc missing) and the
# real gh-pages/channels.meta.json:
#   1. Versions come from each suite's Packages (first stanza of the
#      package, other packages and continuation lines ignored) and
#      updated_at from its Release Date; a missing suite gives null.
#   2. The output parses with `python3 -m json.tool` and keeps the
#      metadata's order, default, aliases, labels and installer block.
#   3. A run whose only difference would be generated_at leaves the file
#      byte-for-byte unchanged; a new version rewrites it.
#   4. Packages.gz is read when Packages is missing.
#   5. Invalid metadata (unsafe or duplicate ids, bad default, schema,
#      labels) or an invalid package version makes the generator fail and
#      leaves the existing output untouched.
#
# Run via: scripts/bz test --config=x86 //test:test_gen_channels_json
# or directly: sh test/test_gen_channels_json.sh
set -eu

if [ -n "${TEST_SRCDIR:-}" ]; then
    ROOT="${TEST_SRCDIR}/_main"
else
    ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fi
GEN="$ROOT/scripts/gen_channels_json.py"
META="$ROOT/gh-pages/channels.meta.json"
FIXTURE="$ROOT/test/testdata/apt"
TMP="${TEST_TMPDIR:-/tmp}/gen_channels_json_$$"
mkdir -p "$TMP/out"

trap 'rm -rf "$TMP"' EXIT

fail=0
pass() { echo "PASS: $1"; }
bad()  { echo "FAIL: $1"; fail=1; }

check() {
    if [ "$1" = "$2" ]; then
        pass "$3 (got $1)"
    else
        bad "$3 -- expected $2, got $1"
    fi
}

# Prints the JSON value at a dotted path (list indexes are numbers).
jget() {
    python3 -c '
import json, sys
v = json.load(open(sys.argv[1], encoding="utf-8"))
for part in sys.argv[2].split("."):
    v = v[int(part)] if isinstance(v, list) else v[part]
print(json.dumps(v))
' "$1" "$2"
}

gen() {
    python3 "$GEN" "$@"
}

OUT="$TMP/out/channels.json"

# 1 + 2. Real metadata against the fixtures.
if gen --apt-root "$FIXTURE" --meta "$META" --out "$OUT" \
        --now 2026-10-28T06:00:00Z > "$TMP/log" 2>&1; then
    pass "generator succeeds on the fixtures"
else
    bad "generator failed on the fixtures: $(cat "$TMP/log")"
fi
if python3 -m json.tool "$OUT" > /dev/null 2>&1; then
    pass "output parses with python3 -m json.tool"
else
    bad "output does not parse with python3 -m json.tool"
fi
check "$(jget "$OUT" schema)" '1' "schema"
check "$(jget "$OUT" generated_at)" '"2026-10-28T06:00:00Z"' "generated_at from --now"
check "$(jget "$OUT" default)" '"early-access"' "default from the metadata"
check "$(jget "$OUT" channels.0.id)" '"early-access"' "first channel is early-access"
check "$(jget "$OUT" channels.1.id)" '"rc"' "second channel is rc"
check "$(jget "$OUT" channels.2.id)" '"stable"' "third channel is stable"
check "$(jget "$OUT" channels.0.aliases)" '["latest"]' "early-access alias latest"
check "$(jget "$OUT" channels.0.label)" '"Latest (alpha)"' "early-access label"
check "$(jget "$OUT" channels.0.version)" '"1.6.33"' "early-access version from Packages"
check "$(jget "$OUT" channels.0.updated_at)" '"2026-10-22T19:37:09Z"' \
    "early-access updated_at from Release Date"
check "$(jget "$OUT" channels.1.version)" 'null' "missing rc suite gives a null version"
check "$(jget "$OUT" channels.1.updated_at)" 'null' "missing rc suite gives a null updated_at"
check "$(jget "$OUT" channels.2.version)" '"1.6.32"' \
    "stable version from the first onvif-recorder stanza"
check "$(jget "$OUT" channels.2.updated_at)" '"2026-10-23T12:51:39Z"' "stable updated_at"
check "$(jget "$OUT" installer.download_url)" \
    '"https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/releases/latest"' \
    "installer.download_url passed through"
check "$(jget "$OUT" installer.min_version)" 'null' "installer.min_version passed through"
leftovers=$(find "$TMP/out" -name '.channels-*' | wc -l | tr -d ' ')
check "$leftovers" '0' "no temporary files left next to the output"

# 3. Only generated_at would change: the file is kept as it is.
cp "$OUT" "$TMP/first.json"
gen --apt-root "$FIXTURE" --meta "$META" --out "$OUT" \
    --now 2026-10-29T07:00:00Z > "$TMP/log" 2>&1
if cmp -s "$OUT" "$TMP/first.json"; then
    pass "unchanged apt repo and metadata leave the file untouched"
else
    bad "an unchanged run rewrote the file"
fi

APT="$TMP/apt"
cp -RL "$FIXTURE" "$APT"
EA_PACKAGES="$APT/dists/early-access/main/binary-arm64/Packages"
sed 's/^Version: 1\.6\.33$/Version: 1.6.34/' "$EA_PACKAGES" > "$TMP/Packages.new"
rm -f "$EA_PACKAGES"
mv "$TMP/Packages.new" "$EA_PACKAGES"
gen --apt-root "$APT" --meta "$META" --out "$OUT" \
    --now 2026-10-29T07:00:00Z > "$TMP/log" 2>&1
check "$(jget "$OUT" channels.0.version)" '"1.6.34"' "a new version is written"
check "$(jget "$OUT" generated_at)" '"2026-10-29T07:00:00Z"' \
    "a rewrite updates generated_at"

# 4. Packages.gz when Packages is missing.
ST_PACKAGES="$APT/dists/stable/main/binary-arm64/Packages"
gzip -c "$ST_PACKAGES" > "$ST_PACKAGES.gz"
rm -f "$ST_PACKAGES"
gen --apt-root "$APT" --meta "$META" --out "$OUT" > "$TMP/log" 2>&1
check "$(jget "$OUT" channels.2.version)" '"1.6.32"' "version read from Packages.gz"

# 5. Failures leave the existing output untouched.
cp "$OUT" "$TMP/good.json"

write_meta() {  # file, channels JSON array, default
    printf '{"schema": 1, "default": "%s", "channels": %s}\n' "$3" "$2" > "$1"
}

expect_fail() {  # description, meta file, [apt root]
    if gen --apt-root "${3:-$FIXTURE}" --meta "$2" --out "$OUT" \
            > "$TMP/log" 2>&1; then
        bad "$1: generator succeeded"
    elif ! cmp -s "$OUT" "$TMP/good.json"; then
        bad "$1: output changed after a failure"
    else
        pass "$1"
    fi
}

M="$TMP/meta.json"
write_meta "$M" '[{"id": "early-access; rm -rf /", "label": "X"}]' 'early-access; rm -rf /'
expect_fail "id with shell metacharacters is rejected" "$M"
write_meta "$M" '[{"id": "Stable", "label": "X"}]' 'Stable'
expect_fail "uppercase id is rejected" "$M"
write_meta "$M" '[{"id": "r", "label": "X"}]' 'r'
expect_fail "one-character id is rejected" "$M"
write_meta "$M" '[{"id": "-rc", "label": "X"}]' '-rc'
expect_fail "id starting with '-' is rejected" "$M"
write_meta "$M" '[{"id": "rc-", "label": "X"}]' 'rc-'
expect_fail "id ending with '-' is rejected" "$M"
write_meta "$M" '[{"id": "rc\n", "label": "X"}]' 'rc'
expect_fail "id with a trailing newline is rejected" "$M"
write_meta "$M" \
    '[{"id": "abcdefghijklmnopqrstuvwxyzabcdefg", "label": "X"}]' \
    'abcdefghijklmnopqrstuvwxyzabcdefg'
expect_fail "33-character id is rejected" "$M"
write_meta "$M" '[{"id": "", "label": "X"}]' ''
expect_fail "empty id is rejected" "$M"
write_meta "$M" '[{"id": "rc", "label": "A"}, {"id": "rc", "label": "B"}]' 'rc'
expect_fail "duplicate id is rejected" "$M"
write_meta "$M" \
    '[{"id": "rc", "label": "A"}, {"id": "stable", "aliases": ["rc"], "label": "B"}]' 'rc'
expect_fail "alias that repeats an id is rejected" "$M"
write_meta "$M" '[{"id": "rc", "aliases": ["Latest"], "label": "A"}]' 'rc'
expect_fail "invalid alias is rejected" "$M"
write_meta "$M" '[{"id": "rc", "label": "A"}]' 'latest'
expect_fail "default that is not a channel id is rejected" "$M"
write_meta "$M" '[{"id": "rc", "label": "12345678901234567890123456789012345678901"}]' 'rc'
expect_fail "label over 40 characters is rejected" "$M"
write_meta "$M" '[{"id": "rc", "label": "two\nlines"}]' 'rc'
expect_fail "label with a newline is rejected" "$M"
write_meta "$M" '[]' 'rc'
expect_fail "empty channel list is rejected" "$M"
printf '{"schema": 2, "default": "rc", "channels": [{"id": "rc", "label": "A"}]}\n' > "$M"
expect_fail "schema 2 is rejected" "$M"
printf '{"default": "rc", "channels": [{"id": "rc", "label": "A"}]}\n' > "$M"
expect_fail "missing schema is rejected" "$M"
expect_fail "missing metadata file is rejected" "$TMP/no-such-meta.json"

sed 's/^Version: 1\.6\.34$/Version: 1.6.34;reboot/' "$EA_PACKAGES" > "$TMP/Packages.bad"
rm -f "$EA_PACKAGES"
mv "$TMP/Packages.bad" "$EA_PACKAGES"
expect_fail "invalid package version is rejected" "$META" "$APT"

if [ "$fail" -ne 0 ]; then
    echo "test_gen_channels_json: FAILED"
    exit 1
fi
echo "test_gen_channels_json: all checks passed"
