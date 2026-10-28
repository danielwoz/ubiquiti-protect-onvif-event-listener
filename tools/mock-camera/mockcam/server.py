"""HTTP front ends: the camera port (ONVIF SOAP and /snapshot.jpg) and the control API."""

import json
import logging
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional, Tuple
from urllib.parse import parse_qs, urlsplit

from . import __version__
from .config import KNOB_DEFAULTS, Settings
from .events import KINDS
from .media import MediaSupervisor
from .onvif import SNAPSHOT_PATH, Camera, Response, check_object_opts
from .soap import iso

log = logging.getLogger("mockcam")

MAX_BODY = 1 << 20

# Scenario kind -> (subject in the picture, AI event emitted when "ai" is set, ClassTypes).
SCENARIOS = {
    "person": ("person", "field", "Human"),
    "vehicle": ("vehicle", "field", "Vehicle"),
    "animal": ("animal", "field", "Animal"),
    "package": ("package", "package", None),
}


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 64


class _Base(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MockCam/" + __version__
    camera: Camera = None  # set on the subclass

    def log_message(self, fmt, *args):  # noqa: N802 - BaseHTTPRequestHandler API
        log.debug("%s %s", self.address_string(), fmt % args)

    def _client(self) -> str:
        return f"{self.client_address[0]}:{self.client_address[1]}"

    def _read_body(self) -> Optional[bytes]:
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            out = b""
            while True:
                size = int(self.rfile.readline().strip().split(b";")[0], 16)
                if size == 0:
                    self.rfile.readline()
                    return out
                out += self.rfile.read(size)
                self.rfile.readline()
                if len(out) > MAX_BODY:
                    return None
        n = int(self.headers.get("Content-Length") or 0)
        if n > MAX_BODY:
            return None
        return self.rfile.read(n) if n else b""

    def _send(self, status: int, body: bytes, ctype: str, headers: Optional[dict] = None,
              head_only: bool = False) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if not head_only:
            self.wfile.write(body)

    def _json(self, status: int, obj) -> None:
        self._send(status, (json.dumps(obj, indent=1, default=str) + "\n").encode(),
                   "application/json")


class CameraHandler(_Base):
    """Port 80 of the camera: SOAP on every /onvif path and the snapshot."""

    def do_POST(self):  # noqa: N802
        path = urlsplit(self.path).path
        body = self._read_body()
        if body is None:
            self._send(413, b"too large\n", "text/plain")
            return
        headers = {k.lower(): v for k, v in self.headers.items()}
        resp: Response = self.camera.handle_soap(path, headers, body, self._client())
        if resp.close:
            self.close_connection = True
            return
        self._send(resp.status, resp.body, resp.headers.pop("Content-Type"), resp.headers)

    def do_GET(self):  # noqa: N802
        self._get(head_only=False)

    def do_HEAD(self):  # noqa: N802
        self._get(head_only=True)

    def _get(self, head_only: bool) -> None:
        path = urlsplit(self.path).path
        if path == SNAPSHOT_PATH:
            self._snapshot(head_only)
        elif path == "/":
            self._send(200, b"MockCam ONVIF simulator: SOAP at /onvif/device_service\n",
                       "text/plain", head_only=head_only)
        else:
            self._send(404, b"not found\n", "text/plain", head_only=head_only)

    def _snapshot(self, head_only: bool) -> None:
        cam = self.camera
        mode = cam.knobs["snapshot_auth"]
        authz = self.headers.get("Authorization")
        entry = {"t": iso(time.time()), "client": self._client(), "method": self.command,
                 "path": SNAPSHOT_PATH, "op": "snapshot",
                 "user_agent": self.headers.get("User-Agent")}
        ok = mode == "none"
        stale = False
        if not ok and authz and authz.lower().startswith("digest ") and mode in ("digest", "any"):
            ok, stale = cam.digest.check(authz, self.command)
            entry["auth"] = "http-digest" if ok else "http-digest-failed"
        elif not ok and authz and authz.lower().startswith("basic ") and mode in ("basic", "any"):
            ok = cam.digest.check_basic(authz)
            entry["auth"] = "http-basic" if ok else "http-basic-failed"
        if not ok:
            # "any" challenges with Digest and also accepts preemptive Basic.
            hdrs = {"WWW-Authenticate": 'Basic realm="MockCam"' if mode == "basic"
                    else cam.digest.challenge(stale)}
            entry["status"] = 401
            entry.setdefault("auth", "missing")
            cam.requests.add(entry)
            self._send(401, b"unauthorized\n", "text/plain", hdrs, head_only)
            return
        img = cam.media.snapshot() if cam.media else read_file(cam.snapshot_file)
        entry["status"] = 200 if img else 503
        entry["bytes"] = len(img or b"")
        cam.requests.add(entry)
        if not img:
            self._send(503, b"no frame yet\n", "text/plain", head_only=head_only)
            return
        self._send(200, img, "image/jpeg", head_only=head_only)


def read_file(path: Optional[str]) -> Optional[bytes]:
    if not path:
        return None
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


class ControlHandler(_Base):
    """The control API on port 8080 (no authentication: keep it on an isolated network)."""

    def do_GET(self):  # noqa: N802
        u = urlsplit(self.path)
        q = {k: v[-1] for k, v in parse_qs(u.query).items()}
        cam = self.camera
        if u.path in ("/", "/mock"):
            self._send(200, HELP.encode(), "text/plain")
        elif u.path == "/healthz":
            h = cam.media.health() if cam.media else {"ok": True, "media": False}
            if not cam.knobs["stream_up"]:
                h["ok"] = True
            self._json(200 if h["ok"] else 503, h)
        elif u.path == "/mock/config":
            self._json(200, cam.knobs.snapshot())
        elif u.path == "/mock/requests":
            try:
                since, limit = int(q.get("since", 0)), int(q.get("limit", 200))
            except ValueError:
                self._json(400, {"error": "since and limit must be integers"})
                return
            self._json(200, cam.requests.query(since, q.get("op", ""), limit))
        elif u.path == "/mock/subscriptions":
            self._json(200, cam.bus.info())
        elif u.path == "/mock/events":
            self._json(200, {"kinds": {k: v.topic for k, v in KINDS.items()},
                             "states": cam.bus.states(), "history": list(cam.bus.history)[-50:]})
        elif u.path == "/mock/state":
            self._json(200, state(cam))
        elif u.path == "/snapshot.jpg":
            img = cam.media.snapshot() if cam.media else read_file(cam.snapshot_file)
            if img:
                self._send(200, img, "image/jpeg")
            else:
                self._send(503, b"no frame yet\n", "text/plain")
        else:
            self._json(404, {"error": f"no such endpoint {u.path}"})

    def do_DELETE(self):  # noqa: N802
        u = urlsplit(self.path)
        cam = self.camera
        if u.path == "/mock/requests":
            cam.requests.clear()
            self._json(200, {"cleared": True})
        elif u.path == "/mock/subscriptions":
            self._json(200, {"dropped": cam.bus.drop_all("dropped by control API")})
        else:
            self._json(404, {"error": f"no such endpoint {u.path}"})

    def do_POST(self):  # noqa: N802
        u = urlsplit(self.path)
        cam = self.camera
        raw = self._read_body()
        try:
            body = json.loads(raw or b"{}")
            if not isinstance(body, dict):
                raise ValueError("body must be a JSON object")
            if u.path == "/mock/config":
                self._json(200, configure(cam, body))
            elif u.path == "/mock/config/reset":
                knobs = cam.knobs.reset()
                apply_side_effects(cam, list(KNOB_DEFAULTS))
                self._json(200, knobs)
            elif u.path == "/mock/event":
                self._json(200, post_event(cam, body))
            elif u.path == "/mock/scenario":
                self._json(200, start_scenario(cam, body))
            else:
                self._json(404, {"error": f"no such endpoint {u.path}"})
        except (ValueError, TypeError, KeyError) as e:
            self._json(400, {"error": str(e)})


def apply_side_effects(cam: Camera, changed) -> None:
    if cam.media and "subject" in changed:
        cam.media.set_subject(cam.knobs["subject"])
    if cam.media and "stream_up" in changed:
        cam.media.refresh_status()


def configure(cam: Camera, body: dict) -> dict:
    knobs, changed = cam.knobs.update(body)
    apply_side_effects(cam, changed)
    return {"knobs": knobs, "changed": changed}


def post_event(cam: Camera, body: dict) -> dict:
    """POST /mock/event: one event, optionally reverting after duration_s."""
    if "raw_xml" in body:
        return cam.emit_raw(str(body["raw_xml"]))
    kind = body.get("kind")
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {', '.join(sorted(KINDS))}")
    stateful = KINDS[kind].state_item is not None
    explicit = "state" in body
    state = body.get("state", True)
    if not isinstance(state, bool):
        raise ValueError("state must be true or false")
    duration = body.get("duration_s")
    if duration is not None and (isinstance(duration, bool)
                                 or not isinstance(duration, (int, float))
                                 or not 0 <= duration <= 86400):
        raise ValueError("duration_s must be a number of seconds, 0..86400")
    if stateful and not explicit and duration is None:
        duration = 5
    opts = {k: body[k] for k in ("rule", "object_id", "bbox", "token", "source", "data",
                                 "topic", "property_op", "value") if k in body}
    if "class" in body:
        opts["cls"] = body["class"]
    first = cam.emit(kind, state, **opts)
    out = {"emitted": [first]}
    if stateful and duration and state:
        def revert():
            time.sleep(float(duration))
            cam.emit(kind, False, **{k: v for k, v in opts.items() if k != "property_op"})
        threading.Thread(target=revert, daemon=True).start()
        out["reverts_in_s"] = float(duration)
    return out


def _scenario_opts(sc: dict, kind: str) -> dict:
    """emit() options for event @p kind of scenario @p sc."""
    if KINDS[kind].object_items:
        return {"cls": sc["class"], "bbox": sc["bbox"]}
    return {}


def _end_scenario_locked(cam: Camera) -> None:
    """Cancels the running scenario; events it has set to true go back to false at once.

    Called with cam.scenario_lock held, so the scenario's own thread cannot emit in between.
    """
    sc = cam.scenario
    cam.scenario_gen += 1
    cam.scenario = None
    if sc and sc["phase"] == "active":
        for e in sc["events"]:
            cam.emit(e, False, **_scenario_opts(sc, e))


def start_scenario(cam: Camera, body: dict) -> dict:
    """POST /mock/scenario: show a subject, then emit its events for the given seconds."""
    kind = body.get("kind")
    if kind == "none":
        with cam.scenario_lock:
            _end_scenario_locked(cam)
            cam.set_subject("none")
        return {"cleared": True}
    if kind not in SCENARIOS:
        raise ValueError(f"kind must be one of {', '.join(SCENARIOS)} or none")
    seconds = float(body.get("seconds", 10))
    lead = float(body.get("lead_s", 3.0))
    tail = float(body.get("tail_s", 2.0))
    if not 0 < seconds <= 600 or not 0 <= lead <= 30 or not 0 <= tail <= 60:
        raise ValueError("seconds must be 0..600, lead_s 0..30, tail_s 0..60")
    subject, ai_kind, cls = SCENARIOS[kind]
    events = list(body.get("events", ["cell_motion", "motion_alarm"]))
    if body.get("ai"):
        events.append(body.get("ai_kind", ai_kind))
    for e in events:
        if e not in KINDS or KINDS[e].state_item is None:
            raise ValueError(f"scenario events must be stateful kinds, not {e!r}")
    bbox = body.get("bbox", [-0.4, -0.8, 0.4, 0.9])
    check_object_opts(cls, bbox)

    with cam.scenario_lock:
        _end_scenario_locked(cam)
        gen = cam.scenario_gen
        sc = {"kind": kind, "events": events, "seconds": seconds, "class": cls or "",
              "bbox": bbox, "started": iso(time.time()), "phase": "lead"}
        cam.scenario = sc
        cam.set_subject(subject)

    def run():
        # Each phase change happens under the lock and only while this scenario is current;
        # a scenario that was cancelled has already had its events set back to false.
        time.sleep(lead)
        with cam.scenario_lock:
            if cam.scenario_gen != gen:
                return
            sc["phase"] = "active"
            for e in events:
                cam.emit(e, True, **_scenario_opts(sc, e))
        time.sleep(seconds)
        with cam.scenario_lock:
            if cam.scenario_gen != gen:
                return
            sc["phase"] = "tail"
            for e in events:
                cam.emit(e, False, **_scenario_opts(sc, e))
        time.sleep(tail)
        with cam.scenario_lock:
            if cam.scenario_gen != gen:
                return
            cam.scenario = None
            cam.set_subject("none")

    threading.Thread(target=run, daemon=True).start()
    return {"started": True, "kind": kind, "subject": subject, "events": events,
            "events_start_in_s": lead, "events_stop_in_s": lead + seconds,
            "subject_cleared_in_s": lead + seconds + tail}


def status_text(cam: Camera) -> str:
    k = cam.knobs.snapshot()
    motion = any(s["data"].get("State") == "true" or s["data"].get("IsMotion") == "true"
                 for s in cam.bus.states()
                 if s["topic"] in (KINDS["motion_alarm"].topic, KINDS["cell_motion"].topic))
    sc = cam.scenario
    parts = [f"motion:{'ON' if motion else 'off'}", f"subject:{k['subject']}",
             f"subs:{cam.bus.live_count()}"]
    if sc:
        parts.append(f"scenario:{sc['kind']}/{sc.get('phase')}")
    if k["ptz"]:
        x, y, z, _ = cam.ptz.position()
        parts.append(f"ptz:{x:+.2f},{y:+.2f},{z:.2f}")
    # drawtext expands %{...}; a literal percent sign must be doubled.
    return "  ".join(parts).replace("%", "%%")


def state(cam: Camera) -> dict:
    return {
        "version": __version__,
        "uptime_s": round(time.time() - cam.started),
        "camera_time": iso(cam.now()),
        "identity": {"manufacturer": cam.s.manufacturer, "model": cam.s.model,
                     "serial": cam.s.serial, "mac": cam.s.mac,
                     "advertise_host": cam.s.advertise_host},
        "knobs": cam.knobs.snapshot(),
        "encoders": {k: {x: v[x] for x in ("width", "height", "fps", "bitrate", "gov",
                                          "profile")} for k, v in cam._encoders().items()},
        "scenario": cam.scenario,
        "event_states": cam.bus.states(),
        "subscriptions": cam.bus.info(),
        "media": cam.media.state() if cam.media else None,
    }


HELP = """MockCam control API (no authentication)

GET    /healthz                  ready when both RTSP paths are live and the snapshot is fresh
GET    /mock/state               identity, knobs, encoders, event states, subscriptions,
                                 RTSP readers
GET    /mock/config              knobs
POST   /mock/config              {"knob": value, ...}   (see README for every knob)
POST   /mock/config/reset        all knobs back to their defaults
POST   /mock/event               {"kind": "cell_motion", "state": true, "duration_s": 5, ...}
POST   /mock/scenario            {"kind": "person|vehicle|animal|package|none", "seconds": 10}
GET    /mock/events              event kinds, current states and the last 50 emitted messages
GET    /mock/requests?since=N&op=PullMessages&limit=200   SOAP/snapshot request log
DELETE /mock/requests            clear the request log
GET    /mock/subscriptions       live and recently ended pull-point subscriptions
DELETE /mock/subscriptions       drop every subscription (as a camera reboot would)
GET    /snapshot.jpg             latest stream1 frame, without authentication
"""


def make_handler(base, cam: Camera):
    return type(base.__name__, (base,), {"camera": cam})


def serve(cam: Camera, settings: Settings) -> Tuple[Server, Server]:
    cam_srv = Server((settings.bind, settings.http_port), make_handler(CameraHandler, cam))
    ctl_srv = Server((settings.bind, settings.control_port), make_handler(ControlHandler, cam))
    for srv in (cam_srv, ctl_srv):
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    return cam_srv, ctl_srv


def build(settings: Settings) -> Camera:
    cam = Camera(settings)
    if settings.media:
        cam.media = MediaSupervisor(settings, cam._encoders, lambda: cam.knobs["stream_up"],
                                    lambda: status_text(cam))
        cam.media.subject = cam.knobs["subject"]
    threading.Thread(target=cam.bus.reaper, args=(cam.stop,), daemon=True).start()
    return cam


def main() -> None:
    import signal  # pylint: disable=import-outside-toplevel
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    settings = Settings.from_env()
    if not settings.username or not settings.password:
        raise SystemExit("MOCKCAM_USERNAME and MOCKCAM_PASSWORD must be set")
    cam = build(settings)
    serve(cam, settings)
    log.info("MockCam %s: ONVIF on :%d, control on :%d, RTSP on :%d, advertised as %s",
             __version__, settings.http_port, settings.control_port, settings.rtsp_port,
             settings.advertise_host or "(request Host header)")
    signal.signal(signal.SIGTERM, lambda *_: cam.stop.set())
    signal.signal(signal.SIGINT, lambda *_: cam.stop.set())
    if cam.media:
        cam.media.stop = cam.stop
        cam.media.run()
    else:
        cam.stop.wait()

