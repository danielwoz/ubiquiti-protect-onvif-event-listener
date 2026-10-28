#!/usr/bin/env bash
# collect.sh — incremental, read-only pull of the UDM's logs into the log store.
#
# Run by cron every 15 minutes (see README.md).  Each run fetches the journal entries,
# Protect / UniFi OS log lines and API snapshot that are new since the previous run,
# appends them to DATA/<day>/ as zstd frames, then advances the cursor and offsets in
# DATA/state/.  Data is written before state, so a run that dies half way may store a
# few lines twice on the next run but never loses any.  A failed run changes nothing in
# the store except its line in DATA/<day>/collector.tsv; the next run catches up.
set -euo pipefail
umask 077

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
mkdir -p "$RUN"
run_under_timeout 600 "$0" "$@"

exec 9>"$LOCK"
if ! flock -n 9; then
  log "collect: previous run still holds $LOCK; skipping"
  exit 0
fi

started=$(date +%s)
tmp=$(mktemp -d "$RUN/run.XXXXXX")
cleanup() { rm -rf "$tmp"; close_ssh_master; }
trap cleanup EXIT
trap 'exit 124' TERM
# Staging directories left behind by runs killed with SIGKILL.
find "$RUN" -maxdepth 1 -name 'run.*' -type d -mmin +120 -exec rm -rf {} + 2>/dev/null || true

record_run() {  # status note...
  local status=$1 day
  shift
  day=$(date +%F)
  if mkdir -p "$DATA/$day" 2>/dev/null; then
    printf '%s\t%s\t%s\t%s\n' "$(date -Is)" "$status" "$(($(date +%s) - started))" "$*" \
      >>"$DATA/$day/collector.tsv" || true
  fi
}

fail() {
  local n
  n=$(( $(cat "$RUN/failures" 2>/dev/null || echo 0) + 1 ))
  echo "$n" >"$RUN/failures"
  log "collect: FAIL ($n consecutive): $*"
  record_run fail "consecutive=$n $*"
  if (( n % FAILURE_ALERT_EVERY == 0 )); then
    logger -t onvif-router-collect "router log collection failing ($n runs in a row): $*" || true
  fi
  exit 1
}

[[ -d "$DATA" ]] || fail "store $DATA is not reachable"
mkdir -p "$STATE" || fail "cannot create $STATE"
ssh "${SSH_OPTS[@]}" "$ROUTER" true 2>"$tmp/ssh.err" ||
  fail "ssh $ROUTER: $(tr '\n' ' ' <"$tmp/ssh.err")"

mkdir "$tmp/stage"
python3 -I "$HERE/collect.py" --state "$STATE" --stage "$tmp/stage" \
  --retention-days "$RETENTION_DAYS" \
  -- ssh "${SSH_OPTS[@]}" "$ROUTER" 2>"$tmp/py.err" ||
  fail "$(tail -n 3 "$tmp/py.err" | tr '\n' ' ')"

# Commit data: compressed frames appended, snapshots written, gap notes appended.
stage=$tmp/stage
stored=0
commit_file() {  # mode source relative-path
  local mode=$1 src=$2 rel=$3 dst
  mkdir -p "$DATA/$(dirname "$rel")"
  case $mode in
    append)
      dst="$DATA/$rel.zst"
      zstd -q -"$ZSTD_LEVEL" -c -- "$src" >>"$dst" ;;
    new)
      dst="$DATA/$rel.zst"
      zstd -q -19 -c -- "$src" >"$dst.part" && mv -f -- "$dst.part" "$dst" ;;
    plain)
      dst="$DATA/$rel"
      cat -- "$src" >>"$dst" ;;
  esac
}
for mode in append new plain; do
  [[ -d "$stage/$mode" ]] || continue
  while IFS= read -r -d '' f; do
    commit_file "$mode" "$f" "${f#"$stage/$mode/"}" || fail "writing ${f#"$stage/"} to $DATA"
    stored=$((stored + $(stat -c %s -- "$f")))
  done < <(find "$stage/$mode" -type f -print0 | sort -z)
done

# Then state, each file replaced atomically within the state directory.
if [[ -d "$stage/state" ]]; then
  for f in "$stage/state"/*; do
    name=${f##*/}
    cp -- "$f" "$STATE/.$name.new" && mv -f -- "$STATE/.$name.new" "$STATE/$name" ||
      fail "saving state $name"
  done
fi
date -Is >"$STATE/.last_ok.new" && mv -f -- "$STATE/.last_ok.new" "$STATE/last_ok"
echo 0 >"$RUN/failures"

summary=$(jq -r '"journal_lines=\(.journal_lines) journal_bytes=\(.journal_bytes)" +
  " file_bytes=\(.file_bytes) api_bytes=\(.api_bytes) backfill_lines=\(.backfill_lines)" +
  " gaps=\(.gaps)"' "$stage/stats.json")
record_run ok "$summary staged_bytes=$stored"
log "collect: ok $summary staged_bytes=$stored in $(($(date +%s) - started))s"
