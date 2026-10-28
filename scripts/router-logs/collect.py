#!/usr/bin/env python3
"""Fetches new router log data into a staging directory for collect.sh to commit.

Reads, strictly read-only, over the ssh command given after "--":
  * the whole systemd journal after the cursor stored in STATE/journal.cursor,
  * Protect and UniFi OS log files after the byte offsets in STATE/offsets.json,
  * one snapshot of the recorder's loopback API, state files and /proc values.

Nothing in the store is changed here.  The staging directory receives:
  append/<day>/<path>     text to append to DATA/<day>/<path>.zst as a new zstd frame
  new/<day>/api/<t>.json  snapshot to write as DATA/<day>/api/<t>.json.zst
  plain/<day>/gaps.txt    lines to append to DATA/<day>/gaps.txt
  state/<file>            replacement state files
  stats.json              counters for the run log
Lines are filed under the local (this machine's time zone) date of their own timestamp;
a line without one (a continuation line) goes with the line before it.
"""

import argparse
import datetime as dt
import json
import os
import re
import secrets
import shlex
import subprocess
import sys

PROTECT_DIR = "/srv/unifi-protect/logs"
CORE_DIR = "/data/unifi-core/logs"
# (store sub-directory, router directory, base file name).  Each base file X.log is
# rotated by its writer to X1.log and X2.log.
LOG_FILES = [
    ("protect", PROTECT_DIR, "onvif.events.log"),
    ("protect", PROTECT_DIR, "onvif.log"),
    ("protect", PROTECT_DIR, "cameras.thirdParty.log"),
    ("protect", PROTECT_DIR, "cameras.recordingMode.log"),
    ("protect", PROTECT_DIR, "automationManager.log"),
    ("protect", PROTECT_DIR, "events.cameras.log"),
    ("protect", PROTECT_DIR, "errors.log"),
    ("protect", PROTECT_DIR, "thumbnails.log"),
    ("protect", PROTECT_DIR, "notifications.log"),
    ("protect", PROTECT_DIR, "notifications.push.log"),
    ("protect", PROTECT_DIR, "notifications.email.log"),
    ("protect", PROTECT_DIR, "alarmIntegrations.log"),
    ("protect", PROTECT_DIR, "armAlarm.log"),
    ("unifi-core", CORE_DIR, "notifications.log"),
    ("unifi-core", CORE_DIR, "cloud.notifications.log"),
    # Global Alarm Manager: which alarms matched each event and what their actions did.
    # logrotate renames it to uos-agent.log.1.gz, which is not followed: a rotation
    # loses the lines written since the previous run and is recorded in gaps.txt.
    ("unifi-core", CORE_DIR, "uos-agent.log"),
]
HEAD_LEN = 64
# A base-file chunk without any newline is held back (the writer is mid-line) unless it
# is at least this long.
MAX_PARTIAL_LINE = 1 << 20
JOURNAL_FIRST_RUN_SINCE = "-24h"
# Oldest first.  logguard compresses daemon.log.1 itself when /var/log passes 80% full.
DAEMON_LOGS = ["/var/log/daemon.log.4.zst", "/var/log/daemon.log.3.zst",
               "/var/log/daemon.log.2.zst", "/var/log/daemon.log.1.zst",
               "/var/log/daemon.log.1", "/var/log/daemon.log"]
# /api/config entries kept in snapshots: settings only, never URLs that may carry
# credentials (camera_snapshot_urls, thumbnail_hook_url).
CONFIG_KEEP = {
    "trigger_automations", "notify_via_uos", "verbose", "detect", "detect_override",
    "protect_event_proxy_cameras", "patch_onvif_addressing", "max_events_per_hour",
    "coalesce_window_sec", "default_object_type", "disabled_object_types",
    "excluded_cameras", "first_party_cameras", "first_party_camera_models",
    "poll_interval_sec", "momentary_event_sec", "drop_unclassified_motion",
}
CONFIG_FIELDS = ("name", "override", "flag_default")

# ---------------------------------------------------------------------------------------
# Redaction applied to every collected text line.

# What follows a key up to its value in key=value or "key": "value": the key's closing
# quote, ':' or '=', and the value's opening quote.  A quote may be escaped (\") when the
# pair sits inside a JSON string.
_KV_SEP = r"\\?[\"']?\s*[:=]\s*\\?\"?"
# The value: up to a quote, backslash, space or list separator.
_KV_VALUE = r"[^\"\\\s,;&}]+"
_REDACT = [
    (re.compile(r"(?i)(://[^/\s:@]+:)[^@/\s]+@"), r"\1[redacted]@"),
    (re.compile(r"(?i)(wsse:Password[^>]*>)[^<]*"), r"\1[redacted]"),
    (re.compile(r"(?i)(\bBearer\s+)[A-Za-z0-9._~+/=-]+"), r"\1[redacted]"),
    # Authorization keeps its scheme (Basic, Digest, ...) and loses the credential after it.
    (re.compile(r"(?i)(authorization" + _KV_SEP +
                r"(?:(?:basic|bearer|digest|negotiate|ntlm)\s+)?)" + _KV_VALUE),
     r"\1[redacted]"),
    # A cookie header loses its whole value, every name=value pair in it.
    (re.compile(r"(?i)(cookie" + _KV_SEP + r")[^\"\\\r\n]+"), r"\1[redacted]"),
    # Passwords, secrets and keys, and UniFi user ids: an X-UserId header carrying one is a
    # Protect API credential on the router.  A JSON null and a path (sudo's PWD=/root) are
    # left as they are.
    (re.compile(r"(?i)((?:password|passwd|(?<![a-z])pwd|(?<![a-z])pass|secret|api[_-]?key"
                r"|x-userid|(?<![a-z])user_?id)" + _KV_SEP + r")(?!null\b|/)" + _KV_VALUE),
     r"\1[redacted]"),
    # Access tokens, but not ONVIF object tokens such as inputToken or ProfileToken.
    (re.compile(r"(?i)((?<![a-z])(?:access|refresh|auth|session|id|csrf|api)?[_-]?token"
                + _KV_SEP + r")" + _KV_VALUE), r"\1[redacted]"),
]
_REDACT_HINTS = ("pass", "pwd", "token", "secret", "key", "auth", "bearer", "cookie",
                 "userid", "user_id", "@")


def redact(line):
    """Returns LINE with credential-looking values replaced by [redacted]."""
    low = line.lower()
    if not any(k in low for k in _REDACT_HINTS):
        return line
    for pat, rep in _REDACT:
        line = pat.sub(rep, line)
    return line


def redact_tree(obj):
    """Returns parsed JSON OBJ with redact() applied to every string in it."""
    if isinstance(obj, str):
        return redact(obj)
    if isinstance(obj, list):
        return [redact_tree(x) for x in obj]
    if isinstance(obj, dict):
        return {k: redact_tree(v) for k, v in obj.items()}
    return obj


# ---------------------------------------------------------------------------------------
# Day filing.

_TS = re.compile(r"(\d{4}-\d\d-\d\d)T(\d\d):(\d\d):(\d\d)(?:\.\d+)?(Z|[+-]\d\d:?\d\d)")
_day_cache = {}


def local_day(line):
    """Returns the local YYYY-MM-DD of the ISO timestamp LINE starts with, or None."""
    m = _TS.match(line)
    if not m:
        return None
    key = (m.group(1), m.group(2), m.group(3), m.group(5))
    day = _day_cache.get(key)
    if day is None:
        off = m.group(5)
        iso = "%sT%s:%s:00%s" % (m.group(1), m.group(2), m.group(3),
                                 "+00:00" if off == "Z" else off)
        day = dt.datetime.fromisoformat(iso).astimezone().strftime("%Y-%m-%d")
        if len(_day_cache) > 100000:
            _day_cache.clear()
        _day_cache[key] = day
    return day


def file_by_day(lines, carry_day, default_day, min_day=""):
    """Groups LINES by local day.  Returns ({day: [line, ...]}, day of the last line).

    Lines dated before MIN_DAY (outside the store's retention) are dropped.
    """
    out = {}
    day = carry_day or default_day
    for line in lines:
        day = local_day(line) or day
        if day >= min_day:
            out.setdefault(day, []).append(redact(line))
    return out, day


# ---------------------------------------------------------------------------------------
# Remote commands.

class RemoteError(Exception):
    pass


def run_remote(ssh, cmd, stdin_data=None, check=True):
    """Runs CMD (a list) on the router; returns (rc, stdout bytes, stderr text)."""
    p = subprocess.run(ssh + [shlex.join(cmd)], input=stdin_data,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    err = p.stderr.decode("utf-8", "replace")
    if check and p.returncode != 0:
        raise RemoteError("%s: exit %d: %s" % (cmd[0], p.returncode, err.strip()[:300]))
    return p.returncode, p.stdout, err


def decode(data):
    return data.decode("utf-8", "surrogateescape")


def split_lines(text):
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines


# ---------------------------------------------------------------------------------------
# Journal.

def parse_cursor(cur):
    return dict(kv.split("=", 1) for kv in cur.strip().split(";") if "=" in kv)


def to_since(iso_ts):
    """journalctl --since value (router local time) for a short-iso-precise timestamp."""
    t = dt.datetime.fromisoformat(iso_ts.replace("Z", "+00:00")).astimezone()
    return t.strftime("%Y-%m-%d %H:%M:%S")


def check_cursor(ssh, cur):
    """Returns ("ok", None), ("rotated", first-entry cursor and µs) or ("invalid", None)."""
    rc, out, err = run_remote(ssh, ["journalctl", "--cursor=" + cur, "-n", "+1", "-q",
                                    "--no-pager", "-o", "json",
                                    "--output-fields=__CURSOR"], check=False)
    if rc != 0:
        if "seek to cursor" in err:
            return "invalid", None
        raise RemoteError("journalctl cursor check: exit %d: %s" % (rc, err.strip()[:300]))
    text = decode(out).strip()
    if not text:
        return "ok", None
    entry = json.loads(text.splitlines()[0])
    got = parse_cursor(entry["__CURSOR"])
    want = parse_cursor(cur)
    if got.get("s") == want.get("s"):
        try:
            # The stored entry itself, or the one right after it: nothing was lost.
            if int(got["i"], 16) <= int(want["i"], 16) + 1:
                return "ok", None
        except (KeyError, ValueError):
            pass
    return "rotated", (entry["__CURSOR"], int(entry["__REALTIME_TIMESTAMP"]))


def backfill_daemon_log(ssh, start_iso, end_us, stage, run_day, stats):
    """Recovers daemon-facility lines between START_ISO and END_US from /var/log/daemon.log*."""
    start = dt.datetime.fromisoformat(start_iso.replace("Z", "+00:00"))
    end = dt.datetime.fromtimestamp(end_us / 1e6, dt.timezone.utc)
    _, out, _ = run_remote(ssh, ["stat", "-c", "%n %Y"] + DAEMON_LOGS, check=False)
    wanted = []
    for row in decode(out).splitlines():
        path, mtime = row.rsplit(" ", 1)
        if int(mtime) >= start.timestamp():
            wanted.append(path)
    kept = []
    for path in sorted(wanted, key=DAEMON_LOGS.index):
        _, data, _ = run_remote(ssh, ["cat", "--", path])
        if path.endswith(".zst"):
            data = subprocess.run(["zstd", "-dcq"], input=data, stdout=subprocess.PIPE,
                                  check=True).stdout
        for line in split_lines(decode(data)):
            m = _TS.match(line)
            if not m:
                continue
            t = dt.datetime.fromisoformat(m.group(0).replace("Z", "+00:00"))
            if start < t < end:
                kept.append(line)
    by_day, _ = file_by_day(kept, None, run_day)
    for day, lines in by_day.items():
        write_lines(stage, "append", day, "daemon-backfill.log", lines)
    stats["backfill_lines"] = len(kept)
    return len(kept)


def collect_journal(ssh, state_dir, stage, run_day, gaps, stats):
    cur = read_text(os.path.join(state_dir, "journal.cursor"))
    last_ts = read_text(os.path.join(state_dir, "journal.last_ts"))
    base = ["journalctl", "-q", "--no-pager", "-o", "short-iso-precise", "--show-cursor"]
    if not cur:
        sel = ["--since=" + JOURNAL_FIRST_RUN_SINCE]
    else:
        status, info = check_cursor(ssh, cur)
        if status == "ok":
            sel = ["--after-cursor=" + cur]
        elif status == "rotated":
            first_cursor, first_us = info
            first_local = dt.datetime.fromtimestamp(first_us / 1e6).isoformat(
                timespec="seconds")
            n = 0
            if last_ts:
                n = backfill_daemon_log(ssh, last_ts, first_us, stage, run_day, stats)
            gaps.append("journal\tentries after %s were rotated out before collection; "
                        "journal resumes at %s; %d daemon.log line(s) backfilled"
                        % (last_ts or "?", first_local, n))
            sel = ["--cursor=" + first_cursor]
        else:
            gaps.append("journal\tstored cursor unusable; resumed --since %s (entries in that "
                        "second may repeat)" % (last_ts or JOURNAL_FIRST_RUN_SINCE))
            sel = ["--since=" + (to_since(last_ts) if last_ts else JOURNAL_FIRST_RUN_SINCE)]
    _, out, _ = run_remote(ssh, base + sel)
    lines = split_lines(decode(out))
    new_cur = None
    while lines and (lines[-1].startswith("-- cursor: ") or lines[-1] == "-- No entries --"):
        last = lines.pop()
        if last.startswith("-- cursor: ") and new_cur is None:
            new_cur = last[len("-- cursor: "):].strip()
    stats["journal_lines"] = len(lines)
    stats["journal_bytes"] = sum(len(x) + 1 for x in lines)
    if not lines:
        return
    by_day, _ = file_by_day(lines, None, run_day)
    for day, chunk in by_day.items():
        write_lines(stage, "append", day, "journal.log", chunk)
    new_last = next((m.group(0) for m in map(_TS.match, reversed(lines)) if m), None)
    if new_cur:
        write_state(stage, "journal.cursor", new_cur + "\n")
    if new_last:
        write_state(stage, "journal.last_ts", new_last + "\n")


# ---------------------------------------------------------------------------------------
# Rotating log files.

def sibling(path, k):
    """errors.log -> errors1.log for K=1."""
    return path if k == 0 else path[:-len(".log")] + "%d.log" % k


def plan_file(st, files):
    """Decides what to read for one rotating log file.

    ST is the stored {head_len, head_hash, offset} (or None for a file never seen).
    FILES maps generation k (0 = X.log, 1 = X1.log, 2 = X2.log) to
    (size, md5 of the first ST.head_len bytes, md5 of the first 64 bytes).
    Returns (reads, gap) where reads lists (k, start, end) oldest first.
    """
    if 0 not in files:
        return [], None
    size0 = files[0][0]
    if st is None:
        return [(0, 0, size0)], None
    if files[0][1] == st["head_hash"]:
        if size0 >= st["offset"]:
            return [(0, st["offset"], size0)], None
        return [(0, 0, size0)], "shrank below the stored offset; re-read from the start"
    for k in (1, 2):
        if k in files and files[k][1] == st["head_hash"]:
            if files[k][0] < st["offset"]:
                return [(0, 0, size0)], "rotated file %d is shorter than expected" % k
            reads = [(k, st["offset"], files[k][0])]
            reads += [(j, 0, files[j][0]) for j in range(k - 1, 0, -1) if j in files]
            reads.append((0, 0, size0))
            return reads, None
    return [(0, 0, size0)], "rotated past generation 2 or rewritten; read from the start"


def take_complete_lines(data, start, end, is_base):
    """Returns (bytes to keep, new offset) for a chunk read from [START, END)."""
    if not is_base:
        if data and not data.endswith(b"\n"):
            data += b"\n"
        return data, end
    cut = data.rfind(b"\n")
    if cut < 0:
        if len(data) >= MAX_PARTIAL_LINE:
            return data + b"\n", start + len(data)
        return b"", start
    return data[:cut + 1], start + cut + 1


def collect_files(ssh, state_dir, stage, run_day, min_day, gaps, stats, remote_stat):
    offsets = {}
    raw = read_text(os.path.join(state_dir, "offsets.json"))
    if raw:
        offsets = json.loads(raw)
    args = []
    for group, rdir, name in LOG_FILES:
        st = offsets.get("%s/%s" % (group, name))
        n = st["head_len"] if st else HEAD_LEN
        for k in (0, 1, 2):
            args += [str(n), sibling(os.path.join(rdir, name), k)]
    with open(remote_stat, "rb") as f:
        script = f.read()
    _, out, _ = run_remote(ssh, ["bash", "-s", "--"] + args, stdin_data=script)
    stat = {}
    for row in decode(out).splitlines():
        path, size, hn, h64 = row.split("\t")
        stat[path] = (int(size), hn, h64)

    new_offsets = {}
    total = 0
    for group, rdir, name in LOG_FILES:
        key = "%s/%s" % (group, name)
        st = offsets.get(key)
        base = os.path.join(rdir, name)
        files = {k: stat[sibling(base, k)] for k in (0, 1, 2) if sibling(base, k) in stat}
        reads, gap = plan_file(st, files)
        if gap:
            gaps.append("%s\t%s" % (key, gap))
        if 0 not in files:
            if st:
                new_offsets[key] = st
            continue
        offset0 = 0
        carry = st.get("last_day") if st else None
        chunks = []
        for k, start, end in reads:
            data = b""
            if end > start:
                _, data, _ = run_remote(ssh, ["tail", "-c", "+%d" % (start + 1), "--",
                                              sibling(base, k)])
                data = data[:end - start]
            kept, new_off = take_complete_lines(data, start, end, k == 0)
            if k == 0:
                offset0 = new_off
            chunks.append(kept)
        blob = b"".join(chunks)
        total += len(blob)
        by_day, carry = file_by_day(split_lines(decode(blob)), carry, run_day, min_day)
        for day, lines in by_day.items():
            write_lines(stage, "append", day, "%s/%s" % (group, name), lines)
        size0, _, h64 = files[0]
        new_offsets[key] = {"head_len": min(HEAD_LEN, size0), "head_hash": h64,
                            "offset": offset0, "last_day": carry}
    stats["file_bytes"] = total
    write_state(stage, "offsets.json", json.dumps(new_offsets, indent=1, sort_keys=True) + "\n")


# ---------------------------------------------------------------------------------------
# API / state snapshot.

def parse_sections(text, boundary):
    sections = {}
    name = None
    buf = []
    for line in text.split("\n"):
        if line.startswith(boundary + " "):
            if name is not None:
                sections[name] = "\n".join(buf).strip("\n")
            name = line[len(boundary) + 1:].strip()
            buf = []
        elif name is not None:
            buf.append(line)
    if name is not None:
        sections[name] = "\n".join(buf).strip("\n")
    return sections


def as_json(text):
    try:
        return json.loads(text)
    except ValueError:
        return text


def filter_config(text):
    try:
        entries = json.loads(text).get("entries", [])
    except (ValueError, AttributeError):
        return "unparsable"
    return [{f: e.get(f) for f in CONFIG_FIELDS} for e in entries
            if isinstance(e, dict) and e.get("name") in CONFIG_KEEP]


def keep_fields(text, fields):
    out = {}
    for line in text.splitlines():
        k, _, v = line.partition(":")
        if k.strip() in fields:
            out[k.strip()] = v.strip()
    return out


def collect_snapshot(ssh, stage, now, remote_snapshot, stats):
    boundary = "@@onvif-router-logs-" + secrets.token_hex(8)
    with open(remote_snapshot, "rb") as f:
        script = f.read()
    _, out, _ = run_remote(ssh, ["bash", "-s", "--", boundary], stdin_data=script)
    sec = parse_sections(decode(out), boundary)
    if "end" not in sec:
        raise RemoteError("snapshot output incomplete")
    snap = {"collected_at": now.isoformat(timespec="seconds"), "api": {}, "state_files": {},
            "proc": {}}
    for name, body in sec.items():
        kind, _, what = name.partition(":")
        if kind == "api":
            snap["api"][what] = filter_config(body) if what == "config" else as_json(body)
        elif kind == "state":
            snap["state_files"][what] = as_json(body) if what.endswith(".json") else body
        elif what == "meminfo":
            snap["proc"]["meminfo"] = keep_fields(
                body, {"MemTotal", "MemAvailable", "SwapTotal", "SwapFree"})
        elif what == "recorder":
            snap["proc"]["recorder"] = keep_fields(
                body, {"Pid", "State", "VmRSS", "VmHWM", "Threads"})
        elif kind == "proc":
            snap["proc"][what] = body.strip()
    text = json.dumps(redact_tree(snap), indent=1, sort_keys=True) + "\n"
    day = now.strftime("%Y-%m-%d")
    path = os.path.join(stage, "new", day, "api", now.strftime("%H%M%S") + ".json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    stats["api_bytes"] = len(text)


# ---------------------------------------------------------------------------------------
# Staging helpers.

def read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except FileNotFoundError:
        return ""


def write_lines(stage, kind, day, rel, lines):
    path = os.path.join(stage, kind, day, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a", encoding="utf-8", errors="surrogateescape") as f:
        f.write("\n".join(lines) + "\n")


def write_state(stage, name, text):
    path = os.path.join(stage, "state", name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--state", required=True, help="store state directory")
    ap.add_argument("--stage", required=True, help="empty local staging directory")
    ap.add_argument("--retention-days", type=int, default=180,
                    help="drop log lines older than this many days")
    ap.add_argument("ssh", nargs=argparse.REMAINDER, help="-- ssh command and host")
    a = ap.parse_args()
    ssh = a.ssh[1:] if a.ssh[:1] == ["--"] else a.ssh
    if not ssh:
        ap.error("missing ssh command after --")
    here = os.path.dirname(os.path.abspath(__file__))
    now = dt.datetime.now().astimezone()
    run_day = now.strftime("%Y-%m-%d")
    min_day = (now - dt.timedelta(days=a.retention_days)).strftime("%Y-%m-%d")
    gaps = []
    stats = {"journal_lines": 0, "journal_bytes": 0, "file_bytes": 0, "api_bytes": 0,
             "backfill_lines": 0}
    try:
        collect_journal(ssh, a.state, a.stage, run_day, gaps, stats)
        collect_files(ssh, a.state, a.stage, run_day, min_day, gaps, stats,
                      os.path.join(here, "remote_stat.sh"))
        collect_snapshot(ssh, a.stage, now, os.path.join(here, "remote_snapshot.sh"), stats)
    except (RemoteError, OSError, ValueError, KeyError) as e:
        print("collect.py: %s" % e, file=sys.stderr)
        return 1
    if gaps:
        stamp = now.isoformat(timespec="seconds")
        write_lines(a.stage, "plain", run_day, "gaps.txt", ["%s\t%s" % (stamp, g) for g in gaps])
    stats["gaps"] = len(gaps)
    with open(os.path.join(a.stage, "stats.json"), "w", encoding="utf-8") as f:
        json.dump(stats, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
