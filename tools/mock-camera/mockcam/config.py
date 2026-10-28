"""Start-up settings (environment) and the runtime knobs the control API changes."""

import copy
import json
import os
import threading
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

# Every knob, its default and (for strings) the values it accepts.
KNOB_DEFAULTS: Dict[str, Any] = {
    # Services advertised by GetServices / GetCapabilities.  Media1 is always on.
    "media2": False,          # also serve ver20/media at /onvif/media2_service
    "ptz": False,             # ver20/ptz at /onvif/ptz_service
    "imaging": True,          # ver20/imaging at /onvif/imaging_service
    "deviceio": True,         # ver10/deviceIO: one digital input (DI_1), one relay (RO_1)
    "analytics": False,       # ver20/analytics with a cell-motion module
    "analytics_sensitivity": 50,   # 0 makes Protect mark motion "misconfigured"
    "events": True,           # ver10/events (PullPoint); false = camera without events
    "motion_topics": True,    # list motion topics in GetEventProperties' TopicSet
    "audio": False,           # AudioEncoderConfiguration in each profile (Protect hasMic)
    # Authentication of SOAP requests.
    "auth_mode": "wsse",      # wsse | wsse_only | digest | none
    "created_window_s": 0,    # >0: reject UsernameTokens whose Created is further off
    "clock_skew_s": 0,        # added to the camera clock (GetSystemDateAndTime, events)
    # Event service behaviour.
    "slots": 0,               # max live pull-point subscriptions; 0 = unlimited
    "wsa_strict": False,      # Pull/Renew/Unsubscribe without wsa:To + wsa:Action -> 400
    "ref_params_required": False,   # shared subscription address + ReferenceParameters
    "notauthorized_on_create": False,  # CreatePullPointSubscription -> ter:NotAuthorized
    "pull_hold": True,        # hold PullMessages until a message or the Timeout
    "initial_state": True,    # queue Initialized messages on Create
    "fault_next": None,       # see README: inject a fault/delay/close into the next request(s)
    "encoder_set": "apply",   # apply | fault: SetVideoEncoderConfiguration
    # Media.
    "stream_up": True,        # false stops the RTSP publisher (cameras "loses" its streams)
    "snapshot_auth": "digest",   # digest | basic | any | none
    "subject": "none",        # none | person | vehicle | animal | package: picture in the video
}

KNOB_CHOICES = {
    "auth_mode": ("wsse", "wsse_only", "digest", "none"),
    "snapshot_auth": ("digest", "basic", "any", "none"),
    "encoder_set": ("apply", "fault"),
    "subject": ("none", "person", "vehicle", "animal", "package"),
}

SIGNED_KNOBS = ("clock_skew_s",)

FAULT_NEXT_KEYS = {"op", "count", "status", "code", "subcode", "reason", "delay_s", "close"}


def validate_fault_next(v: Any) -> Optional[dict]:
    if v is None:
        return None
    if not isinstance(v, dict) or not set(v) <= FAULT_NEXT_KEYS:
        raise ValueError("fault_next must be null or an object with keys "
                         f"{sorted(FAULT_NEXT_KEYS)}")
    out = {"op": "*", "count": 1, "status": 500, "code": "Receiver", "subcode": "ter:Action",
           "reason": "Injected fault", "delay_s": 0, "close": False}
    out.update(v)
    if not isinstance(out["op"], str) or not isinstance(out["count"], int) or out["count"] < 1:
        raise ValueError("fault_next.op must be a string and fault_next.count >= 1")
    if not isinstance(out["status"], int) or not (out["status"] == 0 or 200 <= out["status"] < 600):
        raise ValueError("fault_next.status must be 0 (no fault) or an HTTP status")
    if not isinstance(out["delay_s"], (int, float)) or not 0 <= out["delay_s"] <= 120:
        raise ValueError("fault_next.delay_s must be 0..120")
    return out


def validate_knobs(update: Dict[str, Any]) -> Dict[str, Any]:
    """Checks a partial knob update.  Raises ValueError with a readable message."""
    out = {}
    for k, v in update.items():
        if k not in KNOB_DEFAULTS:
            raise ValueError(f"unknown knob {k!r}; known: {', '.join(sorted(KNOB_DEFAULTS))}")
        if k == "fault_next":
            out[k] = validate_fault_next(v)
            continue
        default = KNOB_DEFAULTS[k]
        if isinstance(default, bool):
            if not isinstance(v, bool):
                raise ValueError(f"{k} must be true or false")
        elif isinstance(default, int):
            if isinstance(v, bool) or not isinstance(v, int):
                raise ValueError(f"{k} must be an integer")
            if v < 0 and k not in SIGNED_KNOBS:
                raise ValueError(f"{k} must be >= 0")
        elif isinstance(default, str):
            if v not in KNOB_CHOICES.get(k, ()):
                raise ValueError(f"{k} must be one of {', '.join(KNOB_CHOICES[k])}")
        out[k] = v
    return out


def _env_int(name: str, default: int) -> int:
    v = os.environ.get(name, "")
    return int(v) if v.strip() else default


@dataclass
class Settings:
    """Fixed for the life of the process; read from MOCKCAM_* environment variables."""
    username: str = ""
    password: str = ""
    advertise_host: str = ""
    advertise_http_port: int = 80
    advertise_rtsp_port: int = 554
    http_port: int = 8000
    control_port: int = 8080
    rtsp_port: int = 8554
    bind: str = "0.0.0.0"
    run_dir: str = "/tmp/mockcam"
    assets_dir: str = "/opt/mockcam/assets"
    media: bool = True
    pattern: str = "smptehdbars"
    manufacturer: str = "MockCam"
    model: str = "ONVIF-SIM-1"
    firmware: str = "1.0.0"
    serial: str = "MOCKCAM0001"
    hardware_id: str = "MOCKCAM-HW1"
    mac: str = "02:4D:4F:43:4B:01"
    knobs: Dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_env(cls) -> "Settings":
        e = os.environ
        s = cls(
            username=e.get("MOCKCAM_USERNAME", ""),
            password=e.get("MOCKCAM_PASSWORD", ""),
            advertise_host=e.get("MOCKCAM_ADVERTISE_HOST", ""),
            advertise_http_port=_env_int("MOCKCAM_ADVERTISE_HTTP_PORT", 80),
            advertise_rtsp_port=_env_int("MOCKCAM_ADVERTISE_RTSP_PORT", 554),
            http_port=_env_int("MOCKCAM_HTTP_PORT", 8000),
            control_port=_env_int("MOCKCAM_CONTROL_PORT", 8080),
            rtsp_port=_env_int("MOCKCAM_RTSP_PORT", 8554),
            bind=e.get("MOCKCAM_BIND", "0.0.0.0"),
            run_dir=e.get("MOCKCAM_RUN_DIR", "/tmp/mockcam"),
            assets_dir=e.get("MOCKCAM_ASSETS_DIR", "/opt/mockcam/assets"),
            media=e.get("MOCKCAM_MEDIA", "1") not in ("0", "false", "no"),
            pattern=e.get("MOCKCAM_PATTERN", "smptehdbars"),
            manufacturer=e.get("MOCKCAM_MANUFACTURER", "MockCam"),
            model=e.get("MOCKCAM_MODEL", "ONVIF-SIM-1"),
            firmware=e.get("MOCKCAM_FIRMWARE", "1.0.0"),
            serial=e.get("MOCKCAM_SERIAL", "MOCKCAM0001"),
            hardware_id=e.get("MOCKCAM_HARDWARE_ID", "MOCKCAM-HW1"),
            mac=e.get("MOCKCAM_MAC", "02:4D:4F:43:4B:01"),
        )
        raw = e.get("MOCKCAM_KNOBS", "").strip()
        if raw:
            s.knobs = validate_knobs(json.loads(raw))
        return s


# Video encoder configuration per profile.  Resolution lists are what
# GetVideoEncoderConfigurationOptions offers for that profile.
DEFAULT_ENCODERS = {
    "stream1": {"token": "VEC_1", "name": "MainStream", "width": 1280, "height": 720,
                "fps": 10, "bitrate": 800, "gov": 20, "quality": 5, "profile": "Main",
                "resolutions": [(1280, 720), (1024, 576), (960, 544), (640, 360)]},
    "stream2": {"token": "VEC_2", "name": "SubStream", "width": 848, "height": 480,
                "fps": 10, "bitrate": 250, "gov": 20, "quality": 5, "profile": "Main",
                "resolutions": [(848, 480), (640, 360), (424, 240)]},
}

FPS_RANGE = (1, 15)
GOV_RANGE = (1, 150)
BITRATE_RANGE = (64, 4096)
QUALITY_RANGE = (1, 10)


class Knobs:
    """Thread-safe knob store."""

    def __init__(self, initial: Optional[Dict[str, Any]] = None):
        self._lock = threading.Lock()
        self._v = dict(KNOB_DEFAULTS)
        if initial:
            self._v.update(validate_knobs(initial))

    def __getitem__(self, k: str) -> Any:
        with self._lock:
            return self._v[k]

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._v)

    def update(self, update: Dict[str, Any]) -> Tuple[Dict[str, Any], List[str]]:
        """Applies a validated update; returns (all knobs, names that changed)."""
        clean = validate_knobs(update)
        with self._lock:
            changed = [k for k, v in clean.items() if self._v[k] != v]
            self._v.update(clean)
            return copy.deepcopy(self._v), changed

    def reset(self) -> Dict[str, Any]:
        with self._lock:
            self._v = dict(KNOB_DEFAULTS)
            return copy.deepcopy(self._v)

    def take_fault(self, op: str) -> Optional[dict]:
        """Consumes one injected fault matching @p op, if armed."""
        with self._lock:
            f = self._v.get("fault_next")
            if not f or f["op"] not in ("*", op):
                return None
            f = dict(f)
            left = f["count"] - 1
            self._v["fault_next"] = dict(f, count=left) if left > 0 else None
            return f
