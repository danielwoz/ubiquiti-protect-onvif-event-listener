# Router log collection (dev)

Collects the dev router's logs (UDM Pro at `192.168.1.1`) onto ceph-server-01 so
onvif-recorder problems can be investigated after the router's own short-lived logs have
rotated away, and writes a daily Markdown summary.

The router's journal is capped at 80 MB and holds only about 18 hours with `--verbose`
on. Protect's own files rotate at 5 MiB. This collector keeps 180 days of both.

## What runs where

| Piece | Runs on | What it does |
|---|---|---|
| `collect.sh` | ceph-server-01, cron as `danielwoz`, every 15 min at `:07 :22 :37 :52` | Fetches what is new since the last run and appends it to the store. |
| `daily-summary.sh` | ceph-server-01, cron as `danielwoz`, 00:25 | Runs a read-only DB query, writes `summary/<yesterday>.md` and prunes raw data older than 180 days. |
| `collect.py`, `summarize.py` | ceph-server-01 | Helpers for the two scripts above. |
| `remote_stat.sh`, `remote_snapshot.sh` | router, fed through `ssh … bash -s` | Never copied to the router; they are piped to `bash` over ssh. |
| `lib.sh` | — | Shared settings. Every setting can be overridden with a `ROUTER_LOGS_*` environment variable. |

**Read-only on the router.**
- Nothing is written there: no files, not even in `/tmp`, and no state.
- The router is only read, using `journalctl`, `tail`, `head`, `stat`, `cat`, `md5sum`,
  loopback GETs to `127.0.0.1:7891` and one `psql` `SELECT` a day in a read-only
  transaction.
- The collector never calls `/api/pg_stats?reset=1` (it clears the counters) or
  `/api/diagnostic_dump` (it builds a tarball on the router).
- The only side effects are the usual ssh login records (sshd and logind lines in the
  journal). With connection sharing (`ControlMaster`) that is one login per run.

## What is collected

**Journal** — the whole router journal, not only onvif-recorder.
- Read with `journalctl --after-cursor=<cursor> -o short-iso-precise`.
- The cursor is stored on ceph-server-01, in `state/journal.cursor`.
- Each run checks that the entry at the cursor still exists (`journalctl --cursor=… -n +1`).
  If it has rotated away, the run records a gap.
- After a gap it backfills the missing window from `/var/log/daemon.log*`, which holds
  about 3.5 days of daemon-facility lines (onvif-recorder, systemd). The backfilled lines
  go to `daemon-backfill.log.zst`.

**Log files** — read incrementally by byte offset.
- Protect, in `/srv/unifi-protect/logs/`:
  - `onvif.events.log`, `onvif.log`
  - `cameras.thirdParty.log`, `cameras.recordingMode.log`
  - `automationManager.log`, `events.cameras.log`, `errors.log`, `thumbnails.log`
  - `notifications.log`, `notifications.push.log`, `notifications.email.log`
  - `alarmIntegrations.log`, `armAlarm.log`
- UniFi OS, in `/data/unifi-core/logs/`: `notifications.log`, `cloud.notifications.log`,
  `uos-agent.log` (Global Alarm Manager: which alarms each event matched and what their
  actions did). `uos-agent.log` is rotated by logrotate to `uos-agent.log.1.gz`, which the
  collector does not follow, so a rotation loses up to one run's lines and is noted in
  `gaps.txt`.

How the offsets work:
- Each writer rotates `X.log` to `X1.log` and then `X2.log`.
- Rotation is not always a rename, so the collector tracks each file by an md5 of its
  first 64 bytes rather than by inode.
- When `X.log` no longer starts with the stored head, the collector reads the rest of the
  sibling (`X1.log` or `X2.log`) that does, and then reads the new `X.log` from the start.
- Only whole lines are taken. A line still being written waits for the next run.
- The first time a file is seen, it is read in full. Lines older than the retention
  period are dropped.

**API snapshot** — one per run, from the recorder's loopback API and state.
- API: `/api/status`, `camera_health`, `event_proxy`, `recent_events`, `recent_errors`
  and `pg_stats`.
- An allow-list of settings from `/api/config`: for example `trigger_automations`,
  `verbose` and `detect_override`. URL settings that could carry credentials are never
  written.
- State files from `/var/lib/onvif-recorder`: `healer-restarts`,
  `healer-recording-stuck`, `event-proxy-*` and `upstream-subscriptions.json`.
- From `/proc`: load average, uptime, memory, and the recorder's RSS and thread count.

**DB query** — once a day, in `daily-summary.sh`: events per camera, type and
smart-detect type for the day.

**Never collected:**
- `/var/lib/onvif-recorder/protect-user-id`
- `/etc/onvif-recorder/config.json`
- any password or key

Every collected line, and every string in the API snapshots, also goes through a
redaction filter. It replaces these with `[redacted]`:
- passwords (`password`, `passwd`, `pwd`, `pass`), secrets, API keys and access tokens;
- the credential after an `Authorization` scheme (`Basic`, `Bearer`, `Digest`, …), and
  whole `Cookie` values;
- UniFi user ids (`user_id`, `userId`, `X-UserId`), since an `X-UserId` header is a
  Protect API credential on the router. Protect's automation dumps also name users in
  other fields, such as `created_by` and `receivers`; those are kept;
- `wsse:Password` and `user:pass@` in a URL.

## Layout

Store: `/mnt/cephfs/ssd/onvif-router-logs/` (CephFS SSD pool). The collector's files are
mode 0600 and its directories 0700. The store's top directory stays 0755 so the alarm test
sink (uid 10001) can reach `alarm-sink/`.

```
state/
  journal.cursor  journal.last_ts  offsets.json  last_ok        written with tmp + mv
YYYY-MM-DD/                                    local (Australia/Perth) date of each line
  journal.log.zst                              router journal, one zstd frame per run
  daemon-backfill.log.zst                      only after a journal gap
  protect/<file>.log.zst                       Protect logs (their timestamps are UTC)
  unifi-core/<file>.log.zst                    UniFi OS logs
  api/HHMMSS.json.zst                          one snapshot per run
  db-events.tsv                                daily DB query
  collector.tsv                                one line per run: time, ok|fail, seconds, counters
  gaps.txt                                     only when data could not be read contiguously
summary/YYYY-MM-DD.md                          daily summaries (never pruned)
alarm-sink/                                    written by the alarm test sink, not by this collector
```

**Which day a line goes in.**
- Lines are filed by the local date of their own timestamp, not by when they were
  collected. So `2026-10-28/journal.log.zst` holds exactly that day's journal, even for
  lines collected at 00:07 the next morning or caught up after an outage.
- A continuation line (one without a timestamp, such as a JSON dump) stays with the line
  before it.

**Local working files** are in `~/.cache/onvif-router-logs/`:
- `collect.lock`
- the ssh control socket `cm-*`
- staging directories `run.*`
- `failures` (count of consecutive failed runs)
- `cron.log`

They live outside CephFS so a stalled mount cannot hang the lock or ssh.

Volume is about 4–6 MB a day compressed, so roughly 1 GB for 180 days.

## Querying

```bash
cd /mnt/cephfs/ssd/onvif-router-logs

# Read a day's summary
less summary/2026-10-28.md

# onvif-recorder warnings and errors for a day (glog prefix W/E/F, optionally after "<N>")
zstdgrep -E 'onvif-recorder\[[0-9]+\]: (<[0-7]>)?[WEF][0-9]{4} ' 2026-10-28/journal.log.zst

# Everything about one camera across a week
zstdgrep -h '192\.168\.1\.106' 2026-10-2[2-8]/journal.log.zst | less

# Healer and event-sharing activity
zstdgrep -h -e '\[healer\]' -e "handing Protect's event subscription back" 2026-10-*/journal.log.zst

# Recorder alarms, and Protect's side of them
zstdgrep -h '\[alarm\]' 2026-10-28/journal.log.zst
zstdcat 2026-10-28/protect/automationManager.log.zst | grep -v 'No events data found'

# Protect / MSR / recorder restarts as systemd saw them
zstdgrep -hE 'systemd\[1\]: (Started|Stopping) (onvif-recorder|unifi-protect|msr|msp)\.service' \
  2026-10-*/journal.log.zst

# Protect's own view of ONVIF cameras (UTC timestamps)
zstdcat 2026-10-28/protect/onvif.events.log.zst | grep -E 'circuit-breaker|motionDetection'

# Camera health over the day from the API snapshots
for f in 2026-10-28/api/*.json.zst; do
  zstdcat "$f" | jq -r '.collected_at + " " +
    ([.api.camera_health.cameras[] | "\(.host)=\(.events_1h)"] | join(" "))'
done

# Collector runs and gaps
cat 2026-10-28/collector.tsv 2026-10-28/gaps.txt 2>/dev/null
```

`zstdcat` and `zstdgrep` read every appended frame of a file. Any node or pod that mounts
CephFS as this user can read the store.

## Cron

Installed in `danielwoz`'s crontab on ceph-server-01:

```
# onvif-recorder router log collection (scripts/router-logs/README.md)
7-59/15 * * * * /home/danielwoz/ubiquiti-protect-onvif-event-listener/scripts/router-logs/collect.sh >>/home/danielwoz/.cache/onvif-router-logs/cron.log 2>&1
25 0 * * * /home/danielwoz/ubiquiti-protect-onvif-event-listener/scripts/router-logs/daily-summary.sh >>/home/danielwoz/.cache/onvif-router-logs/cron.log 2>&1
```

**Why these times.** The minute offsets avoid the powerwall jobs at `:00` and `:01` and
the router's own `*/10` logrotate run.

**The scripts must stay on `main` in this checkout.** Cron runs them from here, so a
branch without `scripts/router-logs/` would stop collection. That shows up as failed runs
in `cron.log` until `main` is back.

**Stop or start:**

```bash
# Stop: comment the two entries out (or delete them)
t=$(mktemp) && crontab -l >"$t" && sed -i 's|^\([0-9].*scripts/router-logs/\)|#\1|' "$t" &&
  crontab "$t"; rm -f "$t"
# Start again: uncomment them
t=$(mktemp) && crontab -l >"$t" && sed -i 's|^#\([0-9].*scripts/router-logs/\)|\1|' "$t" &&
  crontab "$t"; rm -f "$t"
# One run by hand (safe at any time; it waits for nothing and skips if a run is active)
scripts/router-logs/collect.sh
scripts/router-logs/daily-summary.sh            # yesterday
scripts/router-logs/daily-summary.sh today      # partial summary of today so far
```

After a pause the next run catches up, as long as the journal still holds the stored
cursor (about 18 h). After a longer pause it records a gap and backfills from
`daemon.log`.

## Retention

- `daily-summary.sh` deletes `YYYY-MM-DD/` directories older than 180 days
  (`ROUTER_LOGS_RETENTION_DAYS`).
- `summary/`, `state/` and any directory not named like a date (such as `alarm-sink/`)
  are never pruned.
- `cron.log` is moved to `cron.log.1` once it passes 5 MB.

## Failure handling

**Locking and timeouts.**
- Each run takes `flock -n` on the local lock. If the previous run is still going, the
  new one exits at once.
- Runs are wrapped in `timeout 600`; the summary uses 900. SIGKILL follows 60 s after the
  SIGTERM, in case the shell itself is blocked on a stalled mount.
- The summary waits up to 5 min for the lock, so it never reads a file mid-append.

**A failed run** (router unreachable, ssh error, CephFS unavailable, unexpected output):
- writes nothing to the store except its line in `collector.tsv`;
- does not move the cursor or offsets;
- increments `~/.cache/onvif-router-logs/failures`.

The next successful run catches up and resets the counter. Every 8th consecutive failure
(about 2 h) logs a warning to ceph-server-01's syslog with tag `onvif-router-collect`:

```bash
journalctl -t onvif-router-collect
```

**Order of writes.** Data is written before state. A run killed between the two stores a
few lines twice on the next run, but never loses any.

Each frame is compressed locally before it is appended. An append that fails or is
interrupted by SIGTERM is cut back to the file's previous size. A partial frame would make
`zstdcat` stop there and hide every frame appended after it.

**Gaps** are noted in `YYYY-MM-DD/gaps.txt` and in the summary's *Collector health*
section. A gap is recorded when:
- the journal rotated past the stored cursor (a backfill from `daemon.log` follows);
- the stored cursor became unusable;
- a Protect file was rotated more than twice between runs, or was truncated.

**The summary** reports for the day:
- runs ok and failed, and the longest time without a successful run;
- the current count of consecutive failures;
- the journal's time coverage;
- any store file that could not be decompressed.

## Daily summary contents

**onvif-recorder severity.**
- Counts by glog prefix (`I`/`W`/`E`/`F`), not by journal priority: every recorder line
  has `PRIORITY=6`.
- An optional syslog `<N>` prefix before the glog letter is accepted. A line that has
  only `<N>` is classed by N.
- Errors and warnings per hour.
- Top messages, with numbers and ids collapsed and IPs kept.

**Service restarts.**
- Started, Stopping, Main process exited, Failed and auto-restart lines from systemd for
  onvif-recorder, unifi-protect, msr, msp and related units.
- Router boots.
- Recorder PID and version changes.

**Healer** (`[healer]`).
- Restarts fired, by reason: DB, MSR, featureFlags-drift, patch-reload, recording-stuck.
- Successes and failures, and suppressions (warmup, cooldown, 24 h cap).
- recording-stuck detections and waits.
- Changes to `healer-restarts` during the day.

**Event sharing** (`[event_proxy]`).
- Hand-backs, resumptions and start-up "still handed back" lines.
- Hourly counters summed.
- The mode and shared cameras.

**Alarms and notifications.**
- From the recorder: `trigger_automations`, `[alarm] triggering automation`, POST status
  codes and failures, history rows, cooldown skips.
- From Protect: `automationManager.log` levels and top messages, push notifications by
  type, email lines.
- From UniFi OS: notifications and pushes.
- Requests received by `alarm-sink/`.

**Cameras.** Per IP:
- events in the DB and the highest `events_1h`;
- unreachable cycles, subscribe, renew and pull errors, snapshot failures and rate-limit
  drops;
- the last `alive:` heartbeat.

Also:
- the `[recorder] last 1h:` totals;
- from Protect's `onvif.events.log`: circuit-breaker and `motionDetection` state changes;
- per-file levels and top warnings and errors for the Protect files.

**Database.** Protect events per camera, type and smart-detect type.

**Router.** Load, available memory and swap, the recorder's RSS, and earlyoom or OOM
kills.

**Collector health**, as above.

## Tests

```bash
python3 -B -m unittest discover -s scripts/router-logs -p 'test_*.py'
```

The tests cover:
- the rotation planner and whole-line handling;
- filing lines by local day, including Protect's UTC timestamps;
- redaction;
- the summary's glog and syslog-prefix parsing.

To try the scripts against a scratch store, point them at other directories. Keep
`ROUTER_LOGS_RUN` short, because ssh socket paths must stay under 108 bytes:

```bash
ROUTER_LOGS_DATA=/tmp/orl ROUTER_LOGS_RUN=$HOME/.cache/orl-test scripts/router-logs/collect.sh
```
