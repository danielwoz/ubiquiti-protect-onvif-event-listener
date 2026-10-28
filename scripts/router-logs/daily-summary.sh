#!/usr/bin/env bash
# daily-summary.sh — writes DATA/summary/<day>.md and prunes old raw data.
#
# Usage: daily-summary.sh [YYYY-MM-DD | today | yesterday]   (default: yesterday)
#
# Run by cron at 00:25 for the previous day.  Steps:
#   1. read-only DB query on the router: Protect events per camera and type for the day,
#      saved as DATA/<day>/db-events.tsv;
#   2. summarize.py over DATA/<day>/ -> DATA/summary/<day>.md;
#   3. delete DATA/YYYY-MM-DD/ directories older than RETENTION_DAYS (summaries are kept).
# Holds the collector's lock while it runs so it never reads a half-appended file.
set -euo pipefail
umask 077

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
mkdir -p "$RUN"
run_under_timeout 900 "$0" "$@"

arg=${1:-yesterday}
day=$(date -d "$arg" +%F) || { log "summary: bad day '$arg'"; exit 2; }
[[ -d "$DATA" ]] || { log "summary: store $DATA is not reachable"; exit 1; }

exec 9>"$LOCK"
if ! flock -w 300 9; then
  log "summary: collector lock busy for 5 min; giving up"
  exit 1
fi
trap close_ssh_master EXIT

ddir="$DATA/$day"
mkdir -p "$ddir" "$DATA/summary"

# 1. Events per camera for the local day, read-only transaction.
start_ms=$(( $(date -d "$day 00:00" +%s) * 1000 ))
end_ms=$(( $(date -d "$day 00:00 + 1 day" +%s) * 1000 ))
db_error=""
sql="SELECT c.name, c.host, c.\"isThirdPartyCamera\", e.type,
       coalesce(e.\"smartDetectTypes\"::text, ''), count(*)
FROM events e JOIN cameras c ON c.id = e.\"cameraId\"
WHERE e.start >= $start_ms AND e.start < $end_ms
GROUP BY 1, 2, 3, 4, 5 ORDER BY 3 DESC, 1, 6 DESC;"
psql_cmd="PGOPTIONS='-c default_transaction_read_only=on' psql -X -q -h /run/postgresql \
-p 5433 -U postgres -d unifi-protect -At -F \"\$(printf '\\t')\" -v ON_ERROR_STOP=1"
if ssh "${SSH_OPTS[@]}" "$ROUTER" "$psql_cmd" <<<"$sql" >"$ddir/.db-events.tsv.new" \
     2>"$RUN/db.err"; then
  mv -f "$ddir/.db-events.tsv.new" "$ddir/db-events.tsv"
else
  rm -f "$ddir/.db-events.tsv.new"
  db_error=$(head -c 300 "$RUN/db.err" | tr '\n' ' ')
  log "summary: DB query failed: $db_error"
fi

# 2. Summary.
failures=$(cat "$RUN/failures" 2>/dev/null || echo 0)
last_ok=$(cat "$STATE/last_ok" 2>/dev/null || echo "")
python3 -I "$HERE/summarize.py" --data "$DATA" --day "$day" --failures "$failures" \
  --last-ok "$last_ok" --db-error "$db_error" >"$DATA/summary/.$day.md.new"
mv -f "$DATA/summary/.$day.md.new" "$DATA/summary/$day.md"

# 3. Retention for raw data; summary/, state/ and other directories are never pruned.
cutoff=$(date -d "today - $RETENTION_DAYS days" +%F)
for d in "$DATA"/[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]; do
  [[ -d "$d" ]] || continue
  if [[ "${d##*/}" < "$cutoff" ]]; then
    rm -rf -- "$d"
    log "summary: pruned ${d##*/} (older than $RETENTION_DAYS days)"
  fi
done

# The cron log grows by about one line per run; keep one old generation once it passes 5 MB.
if [[ -f "$RUN/cron.log" ]] && (( $(stat -c %s "$RUN/cron.log") > 5000000 )); then
  mv -f "$RUN/cron.log" "$RUN/cron.log.1"
fi

log "summary: wrote $DATA/summary/$day.md"
