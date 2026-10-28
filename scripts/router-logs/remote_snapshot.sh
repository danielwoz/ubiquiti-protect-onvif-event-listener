# remote_snapshot.sh — runs on the router via `ssh ... bash -s -- BOUNDARY`.
#
# Prints the recorder's loopback API answers, its small state files and a few /proc
# values, each section preceded by a line "BOUNDARY <name>".  Read-only: loopback GETs
# (never /api/pg_stats?reset=1, never /api/diagnostic_dump), cat and shell builtins.
# protect-user-id is deliberately not read.
b=$1
api=http://127.0.0.1:7891/api
sec() { printf '\n%s %s\n' "$b" "$1"; }

for ep in status camera_health event_proxy recent_events recent_errors pg_stats config; do
  sec "api:$ep"
  curl -s -m 10 "$api/$ep" || printf '__fetch_failed__ curl exit %s' "$?"
done

st=/var/lib/onvif-recorder
for f in healer-restarts healer-recording-stuck event-proxy-active event-proxy-drain \
         event-proxy-note event-proxy-state.json upstream-subscriptions.json; do
  if [ -f "$st/$f" ]; then
    sec "state:$f"
    cat -- "$st/$f"
  fi
done

sec proc:loadavg
cat /proc/loadavg
sec proc:uptime
cat /proc/uptime
sec proc:meminfo
cat /proc/meminfo
for d in /proc/[0-9]*; do
  read -r comm 2>/dev/null <"$d/comm" || continue
  if [ "$comm" = onvif-recorder ]; then
    sec "proc:recorder"
    cat -- "$d/status"
    break
  fi
done
sec end
