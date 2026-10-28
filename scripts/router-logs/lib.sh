# lib.sh — settings shared by collect.sh and daily-summary.sh (sourced, not run).
#
# Every setting can be overridden from the environment, e.g. to point a test run at a
# scratch store:  ROUTER_LOGS_DATA=/tmp/x ROUTER_LOGS_RUN=/tmp/x-run scripts/router-logs/collect.sh

ROUTER="${ROUTER_LOGS_HOST:-root@192.168.1.1}"
# Store for collected data (CephFS) and for state that has to travel with it.
DATA="${ROUTER_LOGS_DATA:-/mnt/cephfs/ssd/onvif-router-logs}"
STATE="$DATA/state"
# Local run directory: lock, ssh control socket, staging, failure counter.  Kept off CephFS
# so a stalled mount cannot hang the lock or ssh.
RUN="${ROUTER_LOGS_RUN:-$HOME/.cache/onvif-router-logs}"
LOCK="$RUN/collect.lock"
RETENTION_DAYS="${ROUTER_LOGS_RETENTION_DAYS:-180}"
ZSTD_LEVEL="${ROUTER_LOGS_ZSTD_LEVEL:-15}"
# Consecutive failed runs between syslog warnings (8 x 15 min = 2 h).
FAILURE_ALERT_EVERY="${ROUTER_LOGS_FAILURE_ALERT_EVERY:-8}"

# ssh options; the host goes after them (ssh "${SSH_OPTS[@]}" "$ROUTER" cmd).
# ControlPath uses %C (a hash) because a socket path must stay under 108 bytes.
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15
          -o ServerAliveCountMax=3 -o ControlMaster=auto -o ControlPersist=60
          -o "ControlPath=$RUN/cm-%C")

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { printf '%s %s\n' "$(date -Is)" "$*"; }

# Re-runs the calling script under timeout(1) once, so a hung ssh or a stalled CephFS
# cannot leave a run holding the lock forever.
run_under_timeout() {
  local secs=$1 script=$2
  shift 2
  if [[ -z "${ROUTER_LOGS_UNDER_TIMEOUT:-}" ]]; then
    export ROUTER_LOGS_UNDER_TIMEOUT=1
    exec timeout "$secs" "$script" "$@"
  fi
}

close_ssh_master() {
  ssh "${SSH_OPTS[@]}" -O exit "$ROUTER" >/dev/null 2>&1 || true
}
