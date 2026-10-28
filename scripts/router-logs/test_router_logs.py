#!/usr/bin/env python3
"""Unit tests for the router log collector helpers.

Run: python3 -m unittest discover -s scripts/router-logs -p 'test_*.py'
"""

import os
import sys
import time
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["TZ"] = "Australia/Perth"
time.tzset()

import collect  # noqa: E402
import summarize  # noqa: E402

ST = {"head_len": 64, "head_hash": "old", "offset": 100}


class PlanFileTest(unittest.TestCase):
    def test_new_file_is_read_from_the_start(self):
        self.assertEqual(collect.plan_file(None, {0: (500, "x", "x")}), ([(0, 0, 500)], None))

    def test_same_file_is_read_from_the_offset(self):
        self.assertEqual(collect.plan_file(ST, {0: (300, "old", "old")}),
                         ([(0, 100, 300)], None))

    def test_unchanged_file_reads_nothing(self):
        self.assertEqual(collect.plan_file(ST, {0: (100, "old", "old")}),
                         ([(0, 100, 100)], None))

    def test_rotation_reads_the_rest_of_x1_then_the_new_base(self):
        files = {0: (40, "new", "new"), 1: (5000, "old", "old"), 2: (5000, "older", "older")}
        self.assertEqual(collect.plan_file(ST, files), ([(1, 100, 5000), (0, 0, 40)], None))

    def test_double_rotation_reads_x2_rest_x1_whole_and_base(self):
        files = {0: (40, "n0", "n0"), 1: (5000, "n1", "n1"), 2: (5000, "old", "old")}
        self.assertEqual(collect.plan_file(ST, files),
                         ([(2, 100, 5000), (1, 0, 5000), (0, 0, 40)], None))

    def test_lost_file_is_a_gap(self):
        reads, gap = collect.plan_file(ST, {0: (40, "n0", "n0"), 1: (9, "n1", "n1")})
        self.assertEqual(reads, [(0, 0, 40)])
        self.assertIn("rotated past", gap)

    def test_truncated_file_is_a_gap(self):
        reads, gap = collect.plan_file(ST, {0: (50, "old", "old")})
        self.assertEqual(reads, [(0, 0, 50)])
        self.assertIn("shrank", gap)

    def test_missing_base_reads_nothing(self):
        self.assertEqual(collect.plan_file(ST, {}), ([], None))


class CompleteLinesTest(unittest.TestCase):
    def test_base_keeps_only_whole_lines(self):
        self.assertEqual(collect.take_complete_lines(b"a\nb\npart", 10, 18, True),
                         (b"a\nb\n", 14))

    def test_base_without_newline_waits(self):
        self.assertEqual(collect.take_complete_lines(b"part", 10, 14, True), (b"", 10))

    def test_rotated_file_is_taken_whole(self):
        self.assertEqual(collect.take_complete_lines(b"a\nb", 0, 3, False), (b"a\nb\n", 3))


class DayFilingTest(unittest.TestCase):
    def test_utc_lines_are_filed_by_local_day(self):
        lines = ["2026-10-27T15:59:59.000Z - info: a", "  continuation",
                 "2026-10-27T16:00:00.000Z - info: b"]
        by_day, last = collect.file_by_day(lines, None, "2026-10-28")
        self.assertEqual(by_day["2026-10-27"], lines[:2])
        self.assertEqual(by_day["2026-10-28"], lines[2:])
        self.assertEqual(last, "2026-10-28")

    def test_leading_continuation_uses_carried_day(self):
        by_day, _ = collect.file_by_day(["}"], "2026-10-01", "2026-10-28")
        self.assertEqual(by_day, {"2026-10-01": ["}"]})

    def test_journal_timestamps(self):
        self.assertEqual(collect.local_day("2026-10-28T23:59:59.999999+08:00 host x: y"),
                         "2026-10-28")


class RedactTest(unittest.TestCase):
    def test_redacts_credentials(self):
        for raw, secret in (('{"password": "hunter2"}', "hunter2"),
                            ("rtsp://admin:pw123@192.168.1.5/x", "pw123"),
                            ("<wsse:Password Type='x'>abc</wsse:Password>", "abc"),
                            ("Authorization: Bearer abc.def", "abc.def"),
                            ("apiKey=XYZ123", "XYZ123"),
                            ('{"accessToken": "tok1"}', "tok1"),
                            ("token=tok2&x=1", "tok2"),
                            ("Authorization: Basic YWRtaW46cHc=", "YWRtaW46cHc="),
                            ("Cookie: a=1; unifises=ck2", "ck2"),
                            ("/snapshot.cgi?usr=admin&pwd=pw4", "pw4"),
                            ('{\\"password\\":\\"pw5\\"}', "pw5"),
                            ("X-UserId: uid6", "uid6"),
                            ('        "user_id": "uid7",', "uid7")):
            self.assertNotIn(secret, collect.redact(raw), raw)

    def test_keeps_scheme_nulls_and_paths(self):
        self.assertEqual(collect.redact("Authorization: Basic YWRtaW46cHc="),
                         "Authorization: Basic [redacted]")
        for line in ('  "userId": null,', "sudo: x : PWD=/root ; USER=root",
                     "retrying GET http://x after user_id refresh"):
            self.assertEqual(collect.redact(line), line)

    def test_redacts_strings_inside_snapshot_json(self):
        snap = {"api": {"recent_errors": "HTTP 401 from http://admin:pw8@192.168.1.5/snap",
                        "n": [1, {"u": "rtsp://a:pw9@h/x"}]}}
        text = str(collect.redact_tree(snap))
        self.assertNotIn("pw8", text)
        self.assertNotIn("pw9", text)
        self.assertIn("192.168.1.5", text)

    def test_keeps_onvif_object_tokens(self):
        line = '    "inputToken": "DI_0",'
        self.assertEqual(collect.redact(line), line)

    def test_leaves_ordinary_lines(self):
        line = "[192.168.1.107] camera unreachable after 3 consecutive failures"
        self.assertEqual(collect.redact(line), line)


class SummaryParseTest(unittest.TestCase):
    PREFIX = "2026-10-28T12:21:13.511900+08:00 130Baden onvif-recorder[583434]: "

    def test_glog_severity(self):
        d = summarize.Day("2026-10-28")
        d.journal_line(self.PREFIX + "W1028 12:21:13.511854  583602 onvif_listener.cpp:1390] "
                       "[192.168.1.107] camera unreachable after 3 consecutive failures")
        self.assertEqual(d.rec_sev["W"], 1)
        self.assertEqual(d.cam["192.168.1.107"]["unreachable"], 1)
        self.assertEqual(d.rec_hour["W"][12], 1)

    def test_syslog_priority_prefix_before_glog(self):
        d = summarize.Day("2026-10-28")
        d.journal_line(self.PREFIX + "<3>E1028 12:21:13.511854  583602 wedge_healer.cpp:273] "
                       "[healer] restart exit code 1 (command: x)")
        self.assertEqual(d.rec_sev["E"], 1)
        self.assertEqual(d.healer["restart_failed"], 1)
        self.assertEqual(d.rec_syslog_prefixed, 1)

    def test_syslog_priority_prefix_alone(self):
        d = summarize.Day("2026-10-28")
        d.journal_line(self.PREFIX + "<4>[event_proxy] 192.168.1.109 handing Protect's event "
                       "subscription back to the camera (slot-limited): x")
        self.assertEqual(d.rec_sev["W"], 1)
        self.assertEqual(len(d.handbacks), 1)

    def test_systemd_restart_lines(self):
        d = summarize.Day("2026-10-28")
        d.journal_line("2026-10-28T22:21:45.925474+08:00 130Baden systemd[1]: msr.service: "
                       "Main process exited, code=killed, status=9/KILL")
        d.journal_line("2026-10-28T22:21:47.130742+08:00 130Baden systemd[1]: Started "
                       "msr.service - Media Server Recording Service.")
        self.assertEqual(d.unit["msr"]["exits"], 1)
        self.assertEqual(d.unit["msr"]["starts"], 1)

    def test_alarm_lines(self):
        d = summarize.Day("2026-10-28")
        d.journal_line(self.PREFIX + "I1028 12:21:13.511854  583602 alarm_notifier.cpp:1221] "
                       "[alarm] triggering automation 6a1b for person on F00000FDAED9 "
                       "event_id=6ae1")
        d.journal_line(self.PREFIX + "I1028 12:21:13.611854  583602 alarm_notifier.cpp:770] "
                       "[alarm] POST http://localhost:7080/x → 200")
        self.assertEqual(d.alarm["triggered"], 1)
        self.assertEqual(d.alarm_triggered["6a1b (person)"], 1)
        self.assertEqual(d.alarm_post_codes["200"], 1)
        self.assertEqual(d.alarm["post_failed"], 0)

    def test_healer_recording_stuck(self):
        d = summarize.Day("2026-10-28")
        d.journal_line(self.PREFIX + "E1028 12:21:13.511854  583602 wedge_healer.cpp:262] "
                       "[healer] recording-stuck: Protect has not started recording a "
                       "third-party camera it would now record, and decides that only at "
                       "start-up -- running: systemctl restart unifi-protect")
        self.assertEqual(d.healer["fired:recording-stuck"], 1)
        self.assertEqual(d.healer["recording_stuck_restart"], 1)

    def test_normalise_keeps_ips(self):
        self.assertEqual(summarize.normalise("[192.168.1.107] error 12 (1/3) id E8B72304145D"),
                         "[192.168.1.107] error # (#/#) id <id>")


if __name__ == "__main__":
    unittest.main()
