"""Runs mediamtx (RTSP server, TCP only, Digest auth) and the ffmpeg publisher that
encodes the test pattern into stream1/stream2 and writes the snapshot JPEG."""

import collections
import json
import logging
import os
import shutil
import socket
import subprocess
import threading
import time
import urllib.request
from typing import Callable, Dict, List, Optional

log = logging.getLogger("mockcam.media")

SUBJECTS = ("none", "person", "vehicle", "animal", "package")
FONT = "/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf"
MEDIAMTX_API = "http://127.0.0.1:9997"


def _atomic_write(path: str, data: bytes) -> None:
    tmp = f"{path}.tmp{os.getpid()}"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


class MediaSupervisor:
    """Starts, watches and restarts mediamtx and ffmpeg."""

    def __init__(self, settings, encoders: Callable[[], Dict[str, dict]],
                 stream_up: Callable[[], bool], status_text: Callable[[], str]):
        self.s = settings
        self.encoders = encoders
        self.stream_up = stream_up
        self.status_text = status_text
        self.run_dir = settings.run_dir
        self.snapshot_path = os.path.join(self.run_dir, "snapshot.jpg")
        self.overlay_path = os.path.join(self.run_dir, "overlay.png")
        self.status_path = os.path.join(self.run_dir, "status.txt")
        self.clock_path = os.path.join(self.run_dir, "clock.txt")
        self.mtx: Optional[subprocess.Popen] = None
        self.ff: Optional[subprocess.Popen] = None
        self.ff_log: collections.deque = collections.deque(maxlen=60)
        self.mtx_log: collections.deque = collections.deque(maxlen=60)
        self.restarts = {"mediamtx": 0, "ffmpeg": 0}
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self._restart_ff = threading.Event()
        self.subject = "none"

    # -- files --------------------------------------------------------------------

    def _prepare(self) -> None:
        os.makedirs(self.run_dir, exist_ok=True)
        s = self.s
        users = [
            {"user": "any", "pass": "", "ips": ["127.0.0.1/32", "::1/128"],
             "permissions": [{"action": "publish"}, {"action": "api"}]},
            {"user": s.username, "pass": s.password, "ips": [],
             "permissions": [{"action": "read"}]},
        ]
        # JSON is valid YAML, so the generated config needs no YAML library.
        conf = {
            "logLevel": "info", "logDestinations": ["stdout"],
            "readTimeout": "10s", "writeTimeout": "10s", "writeQueueSize": 512,
            "authMethod": "internal", "authInternalUsers": users,
            "api": True, "apiAddress": "127.0.0.1:9997",
            "metrics": False, "pprof": False, "playback": False,
            "rtsp": True, "rtspTransports": ["tcp"], "rtspEncryption": "no",
            "rtspAddress": f":{s.rtsp_port}", "rtspAuthMethods": ["digest"],
            "rtmp": False, "hls": False, "webrtc": False, "srt": False, "moq": False,
            "paths": {"stream1": {"source": "publisher"}, "stream2": {"source": "publisher"}},
        }
        path = os.path.join(self.run_dir, "mediamtx.yml")
        _atomic_write(path, json.dumps(conf, indent=1).encode())
        os.chmod(path, 0o600)
        _atomic_write(self.clock_path, (
            f"{s.manufacturer} {s.model}   %{{gmtime:%Y-%m-%d %H\\:%M\\:%S}} UTC   "
            "frame %{frame_num}").encode())
        self.set_subject(self.subject)
        self.refresh_status()

    def set_subject(self, subject: str) -> None:
        if subject not in SUBJECTS:
            raise ValueError(f"subject must be one of {', '.join(SUBJECTS)}")
        src = os.path.join(self.s.assets_dir, "subjects", f"{subject}.png")
        tmp = f"{self.overlay_path}.tmp"
        shutil.copyfile(src, tmp)
        os.replace(tmp, self.overlay_path)
        self.subject = subject
        self.refresh_status()

    def refresh_status(self) -> None:
        try:
            _atomic_write(self.status_path, self.status_text().encode())
        except OSError as e:
            log.warning("status text: %s", e)

    # -- processes ----------------------------------------------------------------

    def _pump(self, proc: subprocess.Popen, sink: collections.deque, name: str) -> None:
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip()
            sink.append(f"{time.strftime('%H:%M:%S')} {line}")
            log.info("[%s] %s", name, line)

    def _start_mediamtx(self) -> None:
        self.mtx = subprocess.Popen(
            ["/mediamtx", os.path.join(self.run_dir, "mediamtx.yml")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        threading.Thread(target=self._pump, args=(self.mtx, self.mtx_log, "mediamtx"),
                         daemon=True).start()
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.s.rtsp_port), 0.5).close()
                return
            except OSError:
                time.sleep(0.2)
        log.error("mediamtx did not open its RTSP port")

    def ffmpeg_args(self) -> List[str]:
        enc = self.encoders()
        s1, s2 = enc["stream1"], enc["stream2"]
        pattern = {"smptehdbars": "smptehdbars", "testsrc2": "testsrc2",
                   "testsrc": "testsrc"}.get(self.s.pattern, "smptehdbars")
        font = f"fontfile={FONT}:" if os.path.exists(FONT) else ""
        text = (f"drawtext={font}textfile={self.clock_path}:x=24:y=20:fontsize=30:"
                "fontcolor=white:box=1:boxcolor=black@0.65:boxborderw=8,"
                f"drawtext={font}textfile={self.status_path}:reload=1:x=24:y=h-54:fontsize=26:"
                "fontcolor=yellow:box=1:boxcolor=black@0.65:boxborderw=8")
        graph = (f"[0:v][1:v]overlay=0:0:format=auto,format=yuv420p,{text},split=2[a][b];"
                 f"[a]scale={s1['width']}:{s1['height']},split=2[v1][snap];"
                 f"[b]scale={s2['width']}:{s2['height']}[v2];"
                 "[snap]fps=2[v3]")

        def x264(e: dict, label: str, path: str) -> List[str]:
            kbps = e["bitrate"]
            return ["-map", f"[{label}]", "-c:v", "libx264", "-threads", "2", "-preset", "veryfast",
                    "-tune", "zerolatency", "-profile:v", e["profile"].lower(),
                    "-pix_fmt", "yuv420p", "-r", str(e["fps"]), "-g", str(e["gov"]),
                    "-keyint_min", str(e["gov"]), "-sc_threshold", "0", "-bf", "0",
                    "-b:v", f"{kbps}k", "-maxrate", f"{kbps}k", "-bufsize", f"{kbps * 2}k",
                    "-x264-params", "repeat-headers=1",
                    "-f", "rtsp", "-rtsp_transport", "tcp",
                    f"rtsp://127.0.0.1:{self.s.rtsp_port}/{path}"]

        return (["ffmpeg", "-hide_banner", "-nostats", "-loglevel", "warning",
                 "-filter_complex_threads", "2",
                 "-re", "-f", "lavfi", "-i", f"{pattern}=size=1280x720:rate=10",
                 # The overlay file is re-read for every frame; a one-packet queue keeps a
                 # subject change about 2 s from reaching the snapshot.
                 "-re", "-thread_queue_size", "1", "-probesize", "32", "-f", "image2",
                 "-loop", "1", "-framerate", "10", "-i", self.overlay_path,
                 "-filter_complex", graph]
                + x264(s1, "v1", "stream1") + x264(s2, "v2", "stream2")
                + ["-map", "[v3]", "-c:v", "mjpeg", "-q:v", "4", "-f", "image2", "-update", "1",
                   "-atomic_writing", "1", self.snapshot_path])

    def _start_ffmpeg(self) -> None:
        args = self.ffmpeg_args()
        log.info("starting ffmpeg: %s", " ".join(args))
        self.ff = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL)
        threading.Thread(target=self._pump, args=(self.ff, self.ff_log, "ffmpeg"),
                         daemon=True).start()

    def _stop_ffmpeg(self) -> None:
        p = self.ff
        self.ff = None
        if p and p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()

    def restart_publisher(self, why: str) -> None:
        log.info("restarting ffmpeg: %s", why)
        self._restart_ff.set()

    def run(self) -> None:
        """Supervision loop; returns when stop is set."""
        self._prepare()
        self._start_mediamtx()
        last_status = 0.0
        while not self.stop.is_set():
            if self.mtx.poll() is not None:
                log.error("mediamtx exited with %s; restarting", self.mtx.returncode)
                self.restarts["mediamtx"] += 1
                self._stop_ffmpeg()
                time.sleep(1)
                self._start_mediamtx()
            want = self.stream_up()
            if self._restart_ff.is_set():
                self._restart_ff.clear()
                self._stop_ffmpeg()
            if want and (self.ff is None or self.ff.poll() is not None):
                if self.ff is not None:
                    log.error("ffmpeg exited with %s; restarting", self.ff.returncode)
                    self.restarts["ffmpeg"] += 1
                    time.sleep(1)
                self._start_ffmpeg()
            elif not want and self.ff is not None:
                self._stop_ffmpeg()
            if time.time() - last_status > 1:
                self.refresh_status()
                last_status = time.time()
            self.stop.wait(0.5)
        self._stop_ffmpeg()
        if self.mtx and self.mtx.poll() is None:
            self.mtx.terminate()

    # -- state ----------------------------------------------------------------------

    def api(self, path: str) -> Optional[dict]:
        try:
            with urllib.request.urlopen(MEDIAMTX_API + path, timeout=2) as r:
                return json.loads(r.read())
        except (OSError, ValueError):
            return None

    def snapshot(self) -> Optional[bytes]:
        try:
            with open(self.snapshot_path, "rb") as f:
                return f.read()
        except OSError:
            return None

    def snapshot_age(self) -> Optional[float]:
        try:
            return time.time() - os.path.getmtime(self.snapshot_path)
        except OSError:
            return None

    def health(self) -> dict:
        paths = self.api("/v3/paths/list") or {}
        ready = {p.get("name"): bool(p.get("ready")) for p in paths.get("items", [])}
        age = self.snapshot_age()
        ok = ready.get("stream1") and ready.get("stream2") and age is not None and age < 10
        return {"ok": bool(ok), "paths_ready": ready, "snapshot_age_s":
                round(age, 1) if age is not None else None}

    def state(self) -> dict:
        paths = self.api("/v3/paths/list") or {}
        sessions = self.api("/v3/rtspsessions/list") or {}
        return {
            "health": self.health(),
            "subject": self.subject,
            "ffmpeg_running": bool(self.ff and self.ff.poll() is None),
            "restarts": dict(self.restarts),
            "paths": [{k: p.get(k) for k in ("name", "ready", "tracks", "bytesReceived")}
                      | {"readers": len(p.get("readers") or [])} for p in paths.get("items", [])],
            "rtsp_sessions": [{k: x.get(k) for k in ("remoteAddr", "state", "path", "transport",
                                                     "created", "bytesSent")}
                              for x in sessions.get("items", [])],
            "ffmpeg_log": list(self.ff_log)[-15:],
        }
