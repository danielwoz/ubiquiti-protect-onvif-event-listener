#!/usr/bin/env python3
"""Writes the Markdown summary of one day of collected router logs.

Reads DATA/<day>/ (journal, daemon-log backfill, Protect and UniFi OS log files, API
snapshots, collector run log, gap notes, the DB query result) and DATA/alarm-sink/, and
prints Markdown to stdout.  Severity of onvif-recorder lines comes from the glog prefix in
the message (I/W/E/F), optionally preceded by a syslog "<N>" prefix; the journal PRIORITY
of those lines is always 6 and is not used.
"""

import argparse
import collections
import datetime as dt
import glob
import json
import os
import re
import subprocess
import sys

TOP = 10
LIST_MAX = 25
WATCH_UNITS = ["onvif-recorder", "unifi-protect", "msr", "msp", "ms", "mst", "unifi-core",
               "postgresql@14-protect", "nginx", "onvif-recorder-autoupdate",
               "onvif-recorder-channel", "protect-watchdog", "protect-fixer-once",
               "ai-feature-console"]
ALWAYS_SHOWN_UNITS = ["onvif-recorder", "unifi-protect", "msr", "msp"]

_JOURNAL = re.compile(r"^(\d{4}-\d\d-\d\dT[\d:.]+(?:Z|[+-]\d\d:?\d\d)) (\S+) ([^:]*?): (.*)$")
_IDENT = re.compile(r"^(.*?)(?:\[(\d+)\])?$")
_GLOG = re.compile(r"^(?:<([0-7])>)?([IWEF])\d{4} (\d\d:\d\d:\d\d)\.\d+\s+\d+ ([^\]\s]+)\] ?(.*)$")
_SYSLOG_PRI = re.compile(r"^<([0-7])>\s*(.*)$")
_PROTECT = re.compile(r"^(\d{4}-\d\d-\d\dT[\d:.]+(?:Z|[+-]\d\d:?\d\d)) - (\w+): ?(.*)$")
_IPV4 = re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}(?::\d+)?\b")
_HEX = re.compile(r"\b(?=[0-9a-fA-F]*\d)[0-9a-fA-F]{12,}\b")
_UUID = re.compile(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                   r"[0-9a-fA-F]{12}\b")
_NUM = re.compile(r"\d+")
_KV_INT = re.compile(r"(\w+)=(-?\d+)")
_UNIT_START = re.compile(r"^Started ([\w@.\\-]+)\.service\b")
_UNIT_STOP = re.compile(r"^Stopping ([\w@.\\-]+)\.service\b")
_UNIT_EVENT = re.compile(r"^([\w@.\\-]+)\.service: (Main process exited, .*|Failed with result .*"
                         r"|Scheduled restart job.*)$")
_LEADING_IP = re.compile(r"^\[(\d{1,3}(?:\.\d{1,3}){3})\]")


def normalise(text, limit=170):
    """Collapses numbers and ids so repeats of one message count together; keeps IPs."""
    ips = []

    def keep(m):
        ips.append(m.group(0))
        return "\x00"
    t = _IPV4.sub(keep, text)
    t = _UUID.sub("<uuid>", t)
    t = _HEX.sub("<id>", t)
    t = _NUM.sub("#", t)
    it = iter(ips)
    t = re.sub("\x00", lambda m: next(it), t)
    return t if len(t) <= limit else t[:limit - 1] + "…"


def code(text):
    """Inline Markdown code span that survives backticks and pipes in the text."""
    return "`" + text.replace("`", "'").replace("|", "¦") + "`"


def to_local(ts):
    return dt.datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone()


def hms(ts):
    try:
        return to_local(ts).strftime("%H:%M:%S")
    except ValueError:
        return ts


def zstd_lines(path, problems):
    """Yields the decompressed lines of PATH; notes a damaged file in PROBLEMS."""
    p = subprocess.Popen(["zstd", "-dcq", "--", path], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    for raw in p.stdout:
        yield raw.decode("utf-8", "replace").rstrip("\n")
    err = p.stderr.read().decode("utf-8", "replace").strip()
    if p.wait() != 0:
        problems.append("%s: %s" % (os.path.relpath(path), err[:200] or "zstd failed"))


class Day:
    def __init__(self, day):
        self.day = day
        self.problems = []
        self.journal_lines = 0
        self.journal_first = None
        self.journal_last = None
        self.backfill_lines = 0
        self.rec_sev = collections.Counter()
        self.rec_hour = {s: [0] * 24 for s in "WEF"}
        self.rec_top = {s: collections.Counter() for s in "WEF"}
        self.rec_syslog_prefixed = 0
        self.rec_unparsed = collections.Counter()
        self.fatal_lines = []
        self.unit = {u: collections.Counter() for u in WATCH_UNITS}
        self.unit_times = {u: [] for u in WATCH_UNITS}
        self.unit_exits = {u: collections.Counter() for u in WATCH_UNITS}
        self.boots = []
        self.oom = []
        self.healer = collections.Counter()
        self.healer_lines = []
        self.handbacks = []
        self.resumed = []
        self.still_handed_back = []
        self.proxy_hourly = collections.Counter()
        self.proxy_hours = 0
        self.proxy_other = collections.Counter()
        self.alarm = collections.Counter()
        self.alarm_post_codes = collections.Counter()
        self.alarm_triggered = collections.Counter()
        self.alarm_lines = collections.Counter()
        self.cam = collections.defaultdict(collections.Counter)
        self.cam_alive = {}
        self.cam_renew_more = collections.Counter()
        self.recorder_hourly = collections.Counter()
        self.recorder_hours = 0
        self.pfile = collections.defaultdict(collections.Counter)
        self.ptop = collections.defaultdict(collections.Counter)
        self.circuit = []
        self.motion_det = []
        self.push_types = collections.Counter()
        self.cloud_push = collections.Counter()
        self.snap = []

    # -- journal -------------------------------------------------------------------------

    def journal_line(self, line, backfill=False):
        m = _JOURNAL.match(line)
        if not m:
            return
        ts, _, ident_pid, msg = m.groups()
        if not ts.startswith(self.day) and to_local(ts).strftime("%Y-%m-%d") != self.day:
            return
        if backfill:
            self.backfill_lines += 1
        else:
            self.journal_lines += 1
            if self.journal_first is None:
                self.journal_first = ts
            self.journal_last = ts
        ident, pid = _IDENT.match(ident_pid).groups()
        if ident == "onvif-recorder":
            self.recorder_line(ts, msg)
        elif ident == "systemd" and pid == "1":
            self.systemd_line(ts, msg)
        elif ident == "earlyoom" and ("sending SIG" in msg or "killing" in msg.lower()):
            self.oom.append((ts, msg))
        elif ident == "kernel" and ("Out of memory: Killed process" in msg
                                    or msg.startswith("Linux version")):
            if msg.startswith("Linux version"):
                self.boots.append(ts)
            else:
                self.oom.append((ts, msg))

    def systemd_line(self, ts, msg):
        if msg.startswith("Startup finished in"):
            self.boots.append(ts)
            return
        for pat, kind in ((_UNIT_START, "starts"), (_UNIT_STOP, "stops")):
            m = pat.match(msg)
            if m and m.group(1) in self.unit:
                self.unit[m.group(1)][kind] += 1
                if kind == "starts":
                    self.unit_times[m.group(1)].append(hms(ts)[:5])
                return
        m = _UNIT_EVENT.match(msg)
        if m and m.group(1) in self.unit:
            u, what = m.groups()
            if what.startswith("Main process exited"):
                self.unit[u]["exits"] += 1
                self.unit_exits[u][what.split(", ", 1)[1]] += 1
            elif what.startswith("Failed with result"):
                self.unit[u]["failed"] += 1
            else:
                self.unit[u]["auto_restarts"] += 1

    def recorder_line(self, ts, msg):
        g = _GLOG.match(msg)
        if g:
            pri, sev, _, src, text = g.groups()
            if pri is not None:
                self.rec_syslog_prefixed += 1
        else:
            s = _SYSLOG_PRI.match(msg)
            if not s:
                self.rec_unparsed[normalise(msg, 120)] += 1
                return
            pri, text, src = s.group(1), s.group(2), "-"
            self.rec_syslog_prefixed += 1
            sev = "E" if pri <= "3" else "W" if pri == "4" else "I"
        self.rec_sev[sev] += 1
        hour = int(ts[11:13])
        if sev in "WEF":
            self.rec_hour[sev][hour] += 1
            self.rec_top[sev][src + " " + normalise(text)] += 1
        if sev == "F":
            self.fatal_lines.append((ts, src + " " + text))
        self.recorder_topics(ts, sev, src, text)

    def recorder_topics(self, ts, sev, src, text):
        if text.startswith("[healer]"):
            self.healer_topic(ts, sev, text)
        elif text.startswith("[event_proxy]"):
            self.proxy_topic(ts, sev, text)
        elif text.startswith("[alarm]"):
            self.alarm_topic(ts, sev, text)
        elif text.startswith("[recorder] last 1h:"):
            self.recorder_hours += 1
            for k, v in _KV_INT.findall(text):
                self.recorder_hourly[k] += int(v)
        m = _LEADING_IP.match(text)
        ip = m.group(1) if m else None
        if ip and "] alive: " in text:
            self.cam_alive[ip] = (ts, text.split("] alive: ", 1)[1])
        if sev not in "WEF":
            return
        if ip is None:
            m = _IPV4.search(text)
            ip = m.group(0).split(":")[0] if m else None
        if ip is None:
            return
        c = self.cam[ip]
        if "camera unreachable after" in text:
            c["unreachable"] += 1
        elif "renew error:" in text:
            c["renew_error"] += 1
            m = re.search(r"\((\d+) more since the last warning\)", text)
            if m:
                self.cam_renew_more[ip] += int(m.group(1))
        elif "pull error:" in text:
            c["pull_error"] += 1
        elif "rate limit (" in text and "dropping detection" in text:
            c["rate_limited"] += 1
        elif "snapshot" in text:
            c["snapshot"] += 1
        elif "] error: " in text:
            c["subscribe_error"] += 1
        else:
            c["other_" + sev] += 1

    def healer_topic(self, ts, sev, text):
        h = self.healer
        if "-- running: " in text:
            m = re.match(r"\[healer\] (\S+?)(?::| wedged)", text)
            h["fired:" + (m.group(1) if m else "?")] += 1
        if "restarted successfully" in text:
            h["restart_ok"] += 1
        if "restart exit code" in text:
            h["restart_failed"] += 1
        m = re.search(r"trigger detected but suppressed \((warmup|cooldown|24 h cap)", text)
        if m:
            h["suppressed:" + m.group(1)] += 1
        if "Protect has not started recording a third-party camera" in text:
            h["recording_stuck_restart"] += 1
        if "Protect has still not started recording" in text:
            h["recording_stuck_seen"] += 1
        if "Protect is still not recording" in text:
            h["recording_stuck_waiting"] += 1
        if "featureFlags" in text and sev in "WE":
            h["feature_flags"] += 1
        if sev in "WEF":
            self.healer_lines.append((ts, sev, text))

    def proxy_topic(self, ts, sev, text):
        if "handing Protect's event subscription back to the camera" in text:
            self.handbacks.append((ts, text[len("[event_proxy] "):]))
        elif "sharing events with Protect again" in text:
            self.resumed.append((ts, text[len("[event_proxy] "):]))
        elif "still handed back to the camera" in text:
            self.still_handed_back.append((ts, text[len("[event_proxy] "):]))
        elif text.startswith("[event_proxy] last 1h:"):
            self.proxy_hours += 1
            for k, v in _KV_INT.findall(text):
                self.proxy_hourly[k] += int(v)
        elif sev in "WEF":
            self.proxy_other[sev + " " + normalise(text)] += 1

    def alarm_topic(self, ts, sev, text):
        a = self.alarm
        m = re.match(r"\[alarm\] triggering automation (\S+) for (\S+)", text)
        if m:
            a["triggered"] += 1
            self.alarm_triggered["%s (%s)" % m.groups()] += 1
        m = re.match(r"\[alarm\] POST \S+ → (\d+)", text)
        if m:
            self.alarm_post_codes[m.group(1)] += 1
        if text.startswith("[alarm] POST ") and (" failed: " in text or " HTTP " in text):
            a["post_failed"] += 1
        if text.startswith("[alarm] recorded history"):
            a["history"] += 1
        if text.startswith("[alarm] skipping"):
            a["skipped"] += 1
        if "no notification sent --" in text:
            a["no_notification"] += 1
        if "outside the alarm's schedule" in text:
            a["outside_schedule"] += 1
        if sev in "WEF":
            self.alarm_lines[sev + " " + normalise(text)] += 1

    # -- Protect / UniFi OS files ----------------------------------------------------------

    def file_line(self, name, line, state):
        m = _PROTECT.match(line)
        if not m:
            if state["day_ok"]:
                self.pfile[name]["continuation"] += 1
            return
        ts, level, msg = m.groups()
        state["day_ok"] = to_local(ts).strftime("%Y-%m-%d") == self.day
        if not state["day_ok"]:
            return
        level = level.lower()
        self.pfile[name][level] += 1
        if level in ("warn", "error"):
            self.ptop[name][level[0].upper() + " " + normalise(msg)] += 1
        if name == "protect/onvif.events.log":
            if "circuit-breaker fired" in msg:
                self.circuit.append((ts, msg))
            if re.search(r"motionDetection \S+ -> \S+", msg):
                self.motion_det.append((ts, msg))
        elif name == "protect/notifications.push.log":
            m2 = re.search(r'Sent push notifications for type "([^"]+)"', msg)
            if m2:
                self.push_types[m2.group(1)] += 1
        elif name == "unifi-core/cloud.notifications.log":
            m2 = re.search(r'Sending push notification "([^"]+)" to mobile app "([^"]+)"', msg)
            if m2:
                self.cloud_push["%s → %s" % m2.groups()] += 1


# ---------------------------------------------------------------------------------------
# Readers.

def read_journal(d, ddir):
    for name, backfill in (("journal.log.zst", False), ("daemon-backfill.log.zst", True)):
        path = os.path.join(ddir, name)
        if os.path.exists(path):
            for line in zstd_lines(path, d.problems):
                d.journal_line(line, backfill)


def read_files(d, ddir):
    for group in ("protect", "unifi-core"):
        for path in sorted(glob.glob(os.path.join(ddir, group, "*.log.zst"))):
            name = "%s/%s" % (group, os.path.basename(path)[:-len(".zst")])
            state = {"day_ok": True}
            for line in zstd_lines(path, d.problems):
                d.file_line(name, line, state)


def read_snapshots(d, ddir):
    for path in sorted(glob.glob(os.path.join(ddir, "api", "*.json.zst"))):
        text = "\n".join(zstd_lines(path, d.problems))
        try:
            d.snap.append(json.loads(text))
        except ValueError:
            d.problems.append("%s: not JSON" % os.path.relpath(path))
    d.snap.sort(key=lambda s: s.get("collected_at", ""))


def read_tsv(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [line.rstrip("\n").split("\t") for line in f if line.strip()]
    except FileNotFoundError:
        return None


def read_alarm_sink(data, day):
    rows = []
    for path in sorted(glob.glob(os.path.join(data, "alarm-sink", "requests-*.jsonl"))):
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    try:
                        r = json.loads(line)
                        t = to_local(r["time"])
                    except (ValueError, KeyError):
                        continue
                    if t.strftime("%Y-%m-%d") == day:
                        hdr = {k.lower(): v for k, v in (r.get("headers") or {}).items()}
                        rows.append((t, r.get("method", ""), r.get("path", ""),
                                     hdr.get("x-test", "")))
        except OSError:
            continue
    return rows


# ---------------------------------------------------------------------------------------
# Output.

def counter_list(out, counter, n=TOP, indent=""):
    for text, cnt in counter.most_common(n):
        out.append("%s- %d × %s" % (indent, cnt, code(text)))
    if len(counter) > n:
        rest = sum(counter.values()) - sum(c for _, c in counter.most_common(n))
        out.append("%s- … %d more distinct message(s), %d line(s)" % (indent, len(counter) - n,
                                                                    rest))


def time_list(out, items, n=LIST_MAX, fmt=lambda ts, t: "%s %s" % (hms(ts), code(t))):
    for ts, text in items[:n]:
        out.append("- " + fmt(ts, text))
    if len(items) > n:
        out.append("- … and %d more" % (len(items) - n))


def snapshot_view(d):
    v = {"versions": [], "cams": collections.OrderedDict(), "load": [], "memavail": [],
         "swapfree": [], "rss": [], "threads": [], "pids": [], "healer_restarts": [],
         "stuck": None, "proxy": None, "config": None, "handed_back": collections.Counter()}
    for s in d.snap:
        api = s.get("api", {})
        st = api.get("status")
        if isinstance(st, dict) and st.get("version"):
            if not v["versions"] or v["versions"][-1] != st["version"]:
                v["versions"].append(st["version"])
        ch = api.get("camera_health")
        if isinstance(ch, dict):
            for c in ch.get("cameras", []):
                key = c.get("host") or c.get("id")
                e = v["cams"].setdefault(key, {"name": c.get("name"), "max_1h": 0,
                                               "third_party": c.get("is_third_party"),
                                               "proxy": c.get("event_proxy")})
                e["max_1h"] = max(e["max_1h"], c.get("events_1h") or 0)
                e["last_1h"] = c.get("events_1h")
                e["proxy"] = c.get("event_proxy")
        ep = api.get("event_proxy")
        if isinstance(ep, dict):
            v["proxy"] = ep
            for c in (ep.get("hub") or {}).get("cameras", []):
                if c.get("handed_back"):
                    v["handed_back"][c.get("ip")] += 1
        if isinstance(api.get("config"), list):
            v["config"] = api["config"]
        proc = s.get("proc", {})
        try:
            v["load"].append(float(proc.get("loadavg", "").split()[0]))
        except (IndexError, ValueError):
            pass
        mi = proc.get("meminfo", {})
        for key, field in (("memavail", "MemAvailable"), ("swapfree", "SwapFree")):
            if field in mi:
                v[key].append(int(mi[field].split()[0]))
        rec = proc.get("recorder", {})
        if "VmRSS" in rec:
            v["rss"].append(int(rec["VmRSS"].split()[0]))
        if "Threads" in rec:
            v["threads"].append(int(rec["Threads"]))
        if rec.get("Pid") and (not v["pids"] or v["pids"][-1] != rec["Pid"]):
            v["pids"].append(rec["Pid"])
        sf = s.get("state_files", {})
        hr = str(sf.get("healer-restarts", "")).split()
        if not v["healer_restarts"] or v["healer_restarts"][-1][1] != hr:
            v["healer_restarts"].append((s.get("collected_at", "?"), hr))
        v["stuck"] = sf.get("healer-recording-stuck", v["stuck"])
    return v


def dur(td):
    mins = int(td.total_seconds()) // 60
    return "%dh%02dm" % (mins // 60, mins % 60)


def epoch_list(values):
    out = []
    for x in values:
        try:
            out.append(dt.datetime.fromtimestamp(int(x)).strftime("%m-%d %H:%M"))
        except ValueError:
            out.append(x)
    return ", ".join(out) or "none"


def render(d, args, collector, gaps, db, db_error, sink):
    v = snapshot_view(d)
    day = dt.date.fromisoformat(d.day)
    now = dt.datetime.now().astimezone()
    out = ["# Router logs — %s (%s)" % (d.day, day.strftime("%A")), ""]
    out.append("Generated %s by `scripts/router-logs/daily-summary.sh` from `%s`."
               % (now.strftime("%Y-%m-%d %H:%M"), os.path.join(args.data, d.day)))
    if day >= now.date():
        out.append("**The day is not over yet; counts are partial.**")
    out.append("")

    # Collector health numbers used by the headlines.
    ok_runs = [r for r in collector if len(r) > 1 and r[1] == "ok"]
    fail_runs = [r for r in collector if len(r) > 1 and r[1] == "fail"]
    times = sorted(dt.datetime.fromisoformat(r[0]) for r in ok_runs)
    start = dt.datetime.combine(day, dt.time(0), tzinfo=now.tzinfo)
    end = min(now, start + dt.timedelta(days=1))
    edges = [start] + times + [end]
    longest = max((b - a for a, b in zip(edges, edges[1:])), default=dt.timedelta(0))

    # Headlines.
    heads = []
    sev = d.rec_sev
    heads.append("onvif-recorder: **%d E**, **%d W**, **%d F** (of %d lines)"
                 % (sev["E"], sev["W"], sev["F"], sum(sev.values())))
    rec_starts = d.unit["onvif-recorder"]["starts"]
    heads.append("Restarts: onvif-recorder %d, unifi-protect %d, msr %d, msp %d; router boots %d"
                 % (rec_starts, d.unit["unifi-protect"]["starts"], d.unit["msr"]["starts"],
                    d.unit["msp"]["starts"], len(d.boots)))
    fired = sum(c for k, c in d.healer.items() if k.startswith("fired:"))
    heads.append("Healer: %d restart(s) fired (%d recording-stuck), %d suppressed"
                 % (fired, d.healer["fired:recording-stuck"],
                    sum(c for k, c in d.healer.items() if k.startswith("suppressed:"))))
    heads.append("Event sharing: %d hand-back(s), %d resumed" % (len(d.handbacks), len(d.resumed)))
    heads.append("Alarms: recorder triggered %d automation(s); Protect sent %d push "
                 "notification batch(es); UniFi OS sent %d push(es)"
                 % (d.alarm["triggered"], sum(d.push_types.values()),
                    sum(d.cloud_push.values())))
    bad = ["%s (%s)" % (ip, ", ".join("%s %d" % (k, n) for k, n in sorted(c.items())))
           for ip, c in sorted(d.cam.items()) if c["unreachable"] or c["renew_error"]]
    if bad:
        heads.append("Cameras with errors: " + "; ".join(bad))
    heads.append("Collector: %d ok / %d failed run(s), longest gap %s, %d gap note(s)"
                 % (len(ok_runs), len(fail_runs), dur(longest), len(gaps)))
    if d.journal_first:
        first, last = to_local(d.journal_first), to_local(d.journal_last)
        if first - start > dt.timedelta(minutes=5) or end - last > dt.timedelta(minutes=20):
            heads.append("**Journal covers only %s – %s of this day**"
                         % (first.strftime("%H:%M"), last.strftime("%H:%M")))
    else:
        heads.append("**No journal data for this day**")
    if d.problems:
        heads.append("**Damaged or unreadable files: %d** (see Collector health)"
                     % len(d.problems))
    out.append("## Headlines")
    out.append("")
    out += ["- " + h for h in heads]
    out.append("")

    # Severity.
    out.append("## onvif-recorder log severity")
    out.append("")
    out.append("From the glog prefix of each line (`I`/`W`/`E`/`F`). Lines without one: %d."
               % sum(d.rec_unparsed.values()))
    if d.rec_syslog_prefixed:
        out.append("Lines carrying a syslog `<N>` prefix in the text: %d." % d.rec_syslog_prefixed)
    out.append("")
    out.append("| I | W | E | F |")
    out.append("|---|---|---|---|")
    out.append("| %d | %d | %d | %d |" % (sev["I"], sev["W"], sev["E"], sev["F"]))
    out.append("")
    out.append("By hour:")
    out.append("")
    out.append("| | " + " | ".join("%02d" % h for h in range(24)) + " |")
    out.append("|---" * 25 + "|")
    for s in "WE":
        out.append("| %s | " % s + " | ".join(str(n) for n in d.rec_hour[s]) + " |")
    out.append("")
    for s, title in (("F", "Fatal"), ("E", "Top errors"), ("W", "Top warnings")):
        if d.rec_top[s]:
            out.append("### %s" % title)
            out.append("")
            counter_list(out, d.rec_top[s])
            out.append("")
    if d.fatal_lines:
        out.append("Fatal lines:")
        out.append("")
        time_list(out, d.fatal_lines)
        out.append("")
    if d.rec_unparsed:
        out.append("### Lines without a glog prefix")
        out.append("")
        counter_list(out, d.rec_unparsed, 5)
        out.append("")

    # Restarts.
    out.append("## Service restarts")
    out.append("")
    out.append("From systemd's own lines in the journal.")
    out.append("")
    out.append("| Unit | Started | Stopping | Main process exited | Failed | Auto-restart | "
               "Start times |")
    out.append("|---|---|---|---|---|---|---|")
    for u in WATCH_UNITS:
        c = d.unit[u]
        if u not in ALWAYS_SHOWN_UNITS and not c:
            continue
        times_s = ", ".join(d.unit_times[u][:12]) + (" …" if len(d.unit_times[u]) > 12 else "")
        out.append("| %s | %d | %d | %d | %d | %d | %s |"
                   % (u, c["starts"], c["stops"], c["exits"], c["failed"], c["auto_restarts"],
                      times_s or "–"))
    out.append("")
    for u in WATCH_UNITS:
        if d.unit_exits[u]:
            out.append("- %s exits: %s" % (u, ", ".join(
                "%s ×%d" % (k, n) for k, n in d.unit_exits[u].most_common())))
    if d.boots:
        out.append("- Router boots: " + ", ".join(hms(t) for t in d.boots))
    if len(v["pids"]) > 1:
        out.append("- onvif-recorder PIDs seen in snapshots: " + " → ".join(v["pids"]))
    if len(v["versions"]) > 1:
        out.append("- onvif-recorder version changed: " + " → ".join(v["versions"]))
    elif v["versions"]:
        out.append("- onvif-recorder version: " + v["versions"][0])
    out.append("")

    # Healer.
    out.append("## Healer")
    out.append("")
    h = d.healer
    out.append("- Restarts fired: %d (%s)" % (fired, ", ".join(
        "%s %d" % (k[6:], n) for k, n in sorted(h.items()) if k.startswith("fired:")) or "none"))
    out.append("- Restart results: %d ok, %d failed" % (h["restart_ok"], h["restart_failed"]))
    out.append("- Suppressed: warmup %d, cooldown %d, 24 h cap %d"
               % (h["suppressed:warmup"], h["suppressed:cooldown"], h["suppressed:24 h cap"]))
    out.append("- recording-stuck: %d restart(s), %d detection(s), %d still-not-recording "
               "warning(s)" % (h["recording_stuck_restart"], h["recording_stuck_seen"],
                               h["recording_stuck_waiting"]))
    out.append("- featureFlags warnings: %d" % h["feature_flags"])
    if v["healer_restarts"]:
        out.append("- `healer-restarts` at the day's first snapshot: %s"
                   % epoch_list(v["healer_restarts"][0][1]))
        for at, val in v["healer_restarts"][1:]:
            out.append("  - changed by %s to: %s" % (at[11:16], epoch_list(val)))
    if v["stuck"]:
        out.append("- `healer-recording-stuck` at the end of the day: %s"
                   % code(str(v["stuck"]).strip()[:200]))
    if d.healer_lines:
        out.append("")
        out.append("Healer warnings and errors:")
        out.append("")
        time_list(out, [(ts, "%s %s" % (s, t)) for ts, s, t in d.healer_lines])
    out.append("")

    # Event sharing.
    out.append("## Event sharing (event proxy)")
    out.append("")
    p = v["proxy"]
    if isinstance(p, dict):
        shared = [c.get("ip") for c in (p.get("hub") or {}).get("cameras", []) if c.get("proxied")]
        out.append("- Mode at the last snapshot: %s, port bound: %s, patch: %s, shared cameras: %s"
                   % (p.get("mode"), "yes" if p.get("bound") else "no", p.get("patch"),
                      ", ".join(shared) or "none"))
    out.append("- Hand-backs: %d; sharing resumed: %d; still handed back at start-up: %d"
               % (len(d.handbacks), len(d.resumed), len(d.still_handed_back)))
    if v["handed_back"]:
        out.append("- Snapshots showing a camera handed back: " + ", ".join(
            "%s ×%d" % kv for kv in v["handed_back"].most_common()))
    if d.proxy_hours:
        out.append("- Hourly counters summed over %d `[event_proxy] last 1h:` line(s): %s"
                   % (d.proxy_hours, " ".join("%s=%d" % kv for kv in d.proxy_hourly.items())))
    for title, items in (("Hand-backs", d.handbacks), ("Resumed", d.resumed),
                         ("Still handed back", d.still_handed_back)):
        if items:
            out.append("")
            out.append(title + ":")
            out.append("")
            time_list(out, items)
    if d.proxy_other:
        out.append("")
        out.append("Other event-sharing warnings and errors:")
        out.append("")
        counter_list(out, d.proxy_other)
    out.append("")

    # Alarms.
    out.append("## Alarms and notifications")
    out.append("")
    if v["config"]:
        cfg = {e["name"]: e for e in v["config"]}
        t = cfg.get("trigger_automations")
        if t:
            out.append("- Recorder setting `trigger_automations`: override %s, flag %s"
                       % (code(str(t.get("override"))), code(str(t.get("flag_default")))))
    a = d.alarm
    out.append("- Recorder `[alarm] triggering automation`: %d%s" % (a["triggered"], (
        " (" + ", ".join("%s ×%d" % kv for kv in d.alarm_triggered.most_common(8)) + ")")
        if d.alarm_triggered else ""))
    out.append("- `[alarm] POST` results: %s; failed POSTs: %d"
               % (", ".join("HTTP %s ×%d" % kv for kv in sorted(d.alarm_post_codes.items()))
                  or "none", a["post_failed"]))
    out.append("- `[alarm] recorded history`: %d; skipped (cooldown): %d; outside schedule: %d; "
               "no notification action: %d" % (a["history"], a["skipped"],
                                               a["outside_schedule"], a["no_notification"]))
    am = d.pfile.get("protect/automationManager.log", collections.Counter())
    out.append("- Protect automationManager.log: %d info, %d warn, %d error"
               % (am["info"], am["warn"], am["error"]))
    out.append("- Protect push notifications sent: %s" % (", ".join(
        "%s ×%d" % kv for kv in d.push_types.most_common()) or "none"))
    em = d.pfile.get("protect/notifications.email.log", collections.Counter())
    out.append("- Protect notifications.email.log lines: %d" % sum(em.values()))
    out.append("- UniFi OS push notifications: %s" % (", ".join(
        "%s ×%d" % kv for kv in d.cloud_push.most_common()) or "none"))
    un = d.pfile.get("unifi-core/notifications.log", collections.Counter())
    out.append("- UniFi OS notifications.log: %d info, %d warn, %d error"
               % (un["info"], un["warn"], un["error"]))
    if sink:
        by_path = collections.Counter("%s %s%s" % (m, pth, (" X-Test=" + xt) if xt else "")
                                      for _, m, pth, xt in sink)
        out.append("- Alarm sink (`alarm-sink/`) requests: %d (%s)" % (len(sink), ", ".join(
            "%s ×%d" % kv for kv in by_path.most_common(8))))
    if d.alarm_lines:
        out.append("")
        out.append("Recorder `[alarm]` warnings and errors:")
        out.append("")
        counter_list(out, d.alarm_lines)
    if d.ptop.get("protect/automationManager.log"):
        out.append("")
        out.append("automationManager.log warnings and errors:")
        out.append("")
        counter_list(out, d.ptop["protect/automationManager.log"], 6)
    out.append("")

    # Cameras.
    out.append("## Cameras")
    out.append("")
    db_by_host = collections.Counter()
    for row in db or []:
        if len(row) >= 6 and row[5].isdigit():
            db_by_host[row[1]] += int(row[5])
    hosts = sorted(set(d.cam) | set(v["cams"]) | set(d.cam_alive) | set(db_by_host),
                   key=lambda x: [int(p) if p.isdigit() else 0 for p in x.split(".")])
    out.append("| Camera | Events (DB) | Max events_1h | Unreachable | Subscribe errors"
               " | Renew errors | Pull errors | Snapshot | Rate-limited | Other W/E | Last alive |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for ip in hosts:
        c = d.cam.get(ip, collections.Counter())
        info = v["cams"].get(ip, {})
        name = "%s %s" % (ip, info.get("name") or "")
        if info.get("proxy"):
            name += " (shared)"
        renew = c["renew_error"]
        renew_s = "%d (+%d suppressed)" % (renew, d.cam_renew_more[ip]) if renew else "0"
        alive = d.cam_alive.get(ip)
        alive_s = "%s %s" % (hms(alive[0])[:5], alive[1]) if alive else "–"
        out.append("| %s | %s | %s | %d | %d | %s | %d | %d | %d | %d | %s |"
                   % (name.strip().replace("|", "¦"), db_by_host.get(ip, "–"),
                      info.get("max_1h", "–"), c["unreachable"], c["subscribe_error"],
                      renew_s, c["pull_error"],
                      c["snapshot"], c["rate_limited"], c["other_W"] + c["other_E"],
                      alive_s.replace("|", "¦")))
    out.append("")
    if d.recorder_hours:
        out.append("- `[recorder] last 1h:` summed over %d line(s): %s"
                   % (d.recorder_hours, " ".join("%s=%d" % kv for kv in
                                                 d.recorder_hourly.items())))
    out.append("- Protect ONVIF circuit-breaker fired: %d" % len(d.circuit))
    out.append("- Protect motionDetection state changes: %d" % len(d.motion_det))
    out.append("")
    if d.circuit or d.motion_det:
        out.append("Protect's ONVIF view (`onvif.events.log`):")
        out.append("")
        time_list(out, sorted(d.circuit + d.motion_det))
        out.append("")

    out.append("### Protect and UniFi OS log files")
    out.append("")
    out.append("| File | info | warn | error | other levels | continuation lines |")
    out.append("|---|---|---|---|---|---|")
    for name in sorted(d.pfile):
        c = d.pfile[name]
        other = sum(n for k, n in c.items() if k not in ("info", "warn", "error", "continuation"))
        out.append("| %s | %d | %d | %d | %d | %d |"
                   % (name, c["info"], c["warn"], c["error"], other, c["continuation"]))
    out.append("")
    for name in ("protect/errors.log", "protect/cameras.thirdParty.log",
                 "protect/onvif.events.log", "protect/onvif.log", "protect/events.cameras.log",
                 "protect/thumbnails.log", "protect/cameras.recordingMode.log"):
        if d.ptop.get(name):
            out.append("Top warnings and errors in `%s`:" % name)
            out.append("")
            counter_list(out, d.ptop[name], TOP if name == "protect/errors.log" else 5)
            out.append("")

    # DB.
    out.append("## Protect events in the database")
    out.append("")
    if db_error:
        out.append("Query failed: " + code(db_error))
    elif db is None:
        out.append("No `db-events.tsv` for this day.")
    else:
        out.append("| Camera | Host | Third-party | Type | Smart detect | Count |")
        out.append("|---|---|---|---|---|---|")
        for row in db:
            if len(row) >= 6:
                out.append("| " + " | ".join(x.replace("|", "¦") for x in row[:6]) + " |")
    out.append("")

    # System.
    out.append("## Router")
    out.append("")
    if v["load"]:
        out.append("- Load average (1 min) in snapshots: max %.2f, median %.2f"
                   % (max(v["load"]), sorted(v["load"])[len(v["load"]) // 2]))
    if v["memavail"]:
        out.append("- MemAvailable min %d MiB; SwapFree min %d MiB"
                   % (min(v["memavail"]) // 1024, min(v["swapfree"] or [0]) // 1024))
    if v["rss"]:
        out.append("- onvif-recorder RSS %d–%d MiB, threads max %d"
                   % (min(v["rss"]) // 1024, max(v["rss"]) // 1024, max(v["threads"] or [0])))
    out.append("- earlyoom / OOM actions: %d" % len(d.oom))
    if d.oom:
        time_list(out, d.oom, 10)
    out.append("")

    # Collector.
    out.append("## Collector health")
    out.append("")
    out.append("- Runs: %d ok, %d failed; longest time without a successful run: %s"
               % (len(ok_runs), len(fail_runs), dur(longest)))
    if args.failures:
        out.append("- Consecutive failed runs now: %s; last successful run: %s"
                   % (args.failures, args.last_ok or "unknown"))
    out.append("- Journal lines for the day: %d (%s – %s); daemon.log backfill lines: %d"
               % (d.journal_lines, hms(d.journal_first) if d.journal_first else "–",
                  hms(d.journal_last) if d.journal_last else "–", d.backfill_lines))
    out.append("- API snapshots: %d" % len(d.snap))
    total = 0
    for root, _, files in os.walk(os.path.join(args.data, d.day)):
        total += sum(os.path.getsize(os.path.join(root, f)) for f in files)
    out.append("- Stored for the day: %.1f MB" % (total / 1e6))
    for r in fail_runs[:10]:
        out.append("  - failed %s: %s" % (r[0][11:19], code(" ".join(r[3:])[:200])))
    if gaps:
        out.append("")
        out.append("Gaps:")
        out.append("")
        for g in gaps[:LIST_MAX]:
            out.append("- %s %s" % (g[0][11:19] if g else "", code(" ".join(g[1:]))))
    if d.problems:
        out.append("")
        out.append("Problems reading the store:")
        out.append("")
        out += ["- " + code(x) for x in d.problems[:LIST_MAX]]
    out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data", required=True)
    ap.add_argument("--day", required=True, help="YYYY-MM-DD")
    ap.add_argument("--failures", default="", help="current consecutive collector failures")
    ap.add_argument("--last-ok", default="", help="time of the last successful collection")
    ap.add_argument("--db-error", default="", help="why the DB query has no result")
    args = ap.parse_args()
    ddir = os.path.join(args.data, args.day)
    d = Day(args.day)
    read_journal(d, ddir)
    read_files(d, ddir)
    read_snapshots(d, ddir)
    collector = read_tsv(os.path.join(ddir, "collector.tsv")) or []
    gaps = read_tsv(os.path.join(ddir, "gaps.txt")) or []
    db = read_tsv(os.path.join(ddir, "db-events.tsv"))
    sink = read_alarm_sink(args.data, args.day)
    sys.stdout.write(render(d, args, collector, gaps, db, args.db_error, sink))
    return 0


if __name__ == "__main__":
    sys.exit(main())
