"""The simulated camera: ONVIF device, media, media2, events, deviceIO, PTZ, imaging and
analytics services, the request log and the control operations behind the control API."""

import collections
import copy
import threading
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple
from urllib.parse import urlsplit

from .config import (BITRATE_RANGE, DEFAULT_ENCODERS, FPS_RANGE, GOV_RANGE, QUALITY_RANGE,
                     Knobs, Settings)
from .events import CLASS_TYPES, KINDS, EventBus, Notification, topic_set_xml
from .soap import (DAHUA_SLOT_FAULT, NS, DigestAuth, SoapParseError, SoapRequest,
                   check_username_token, child, children, envelope, esc, fault, find_text, iso,
                   local, parse_datetime, parse_duration, parse_request, termination_seconds)

PATHS = {
    "device": "/onvif/device_service",
    "media": "/onvif/media_service",
    "media2": "/onvif/media2_service",
    "events": "/onvif/event_service",
    "deviceio": "/onvif/deviceio_service",
    "ptz": "/onvif/ptz_service",
    "imaging": "/onvif/imaging_service",
    "analytics": "/onvif/analytics_service",
}
SUBSCRIPTION_PATH = "/onvif/subscription"
SNAPSHOT_PATH = "/snapshot.jpg"

SERVICE_NS = {
    "device": NS["tds"], "media": NS["trt"], "media2": NS["tr2"], "events": NS["tev"],
    "deviceio": NS["tmd"], "ptz": NS["tptz"], "imaging": NS["timg"], "analytics": NS["tan"],
}
# Services behind a knob of the same name.
OPTIONAL_SERVICES = ("media2", "events", "deviceio", "ptz", "imaging", "analytics")

# Operations ONVIF allows without credentials that this camera honours (GetSystemDateAndTime
# is what Protect sends first, without credentials, to learn the camera's clock).
PRE_AUTH = {("device", "GetSystemDateAndTime")}

H264_PROFILES = ("Baseline", "Main", "High")

SUBSCRIPTION_OPS = {"PullMessages", "Renew", "Unsubscribe", "SetSynchronizationPoint"}
WSA_OPS = {"PullMessages", "Renew", "Unsubscribe"}


class SoapFault(Exception):
    def __init__(self, status: int, code: str, subcode: str, reason: str, nested: str = "",
                 headers: Optional[Dict[str, str]] = None, body: Optional[bytes] = None):
        super().__init__(reason)
        self.status, self.code, self.subcode, self.reason = status, code, subcode, reason
        self.nested, self.headers, self.body = nested, headers or {}, body

    def payload(self) -> bytes:
        return self.body if self.body is not None else fault(
            self.code, self.subcode, self.reason, self.nested)


def not_implemented() -> SoapFault:
    return SoapFault(400, "Receiver", "ter:ActionNotSupported", "Optional Action Not Implemented")


def invalid_arg(reason: str, nested: str = "") -> SoapFault:
    return SoapFault(400, "Sender", "ter:InvalidArgVal", reason, nested)


@dataclass
class Response:
    status: int
    body: bytes
    headers: Dict[str, str] = field(default_factory=dict)
    close: bool = False


class RequestLog:
    def __init__(self, size: int = 1000):
        self._lock = threading.Lock()
        self._items: collections.deque = collections.deque(maxlen=size)
        self._seq = 0

    def add(self, entry: dict) -> None:
        with self._lock:
            self._seq += 1
            entry["seq"] = self._seq
            self._items.append(entry)

    def query(self, since: int = 0, op: str = "", limit: int = 200) -> dict:
        with self._lock:
            items = [e for e in self._items if e["seq"] > since and (not op or e.get("op") == op)]
            return {"last_seq": self._seq, "items": items[-limit:]}

    def clear(self) -> None:
        with self._lock:
            self._items.clear()


def check_object_opts(cls, bbox) -> None:
    """Raises ValueError unless @p cls and @p bbox are valid ClassTypes and BoundingBox values."""
    if cls not in (None, "") and cls not in CLASS_TYPES:
        raise ValueError(f"class must be one of {', '.join(CLASS_TYPES)} or empty")
    if bbox is not None and (not isinstance(bbox, (list, tuple)) or len(bbox) != 4
                             or not all(-1 <= float(v) <= 1 for v in bbox)):
        raise ValueError("bbox must be [left, top, right, bottom] in ONVIF -1..1 coordinates")


def _bool(text: Optional[str]) -> Optional[bool]:
    if text is None:
        return None
    return text.strip().lower() in ("true", "1")


def _num(text: Optional[str], default: float = 0.0) -> float:
    try:
        return float(text) if text not in (None, "") else default
    except ValueError:
        return default


class PtzState:
    def __init__(self):
        self.lock = threading.Lock()
        self.x = self.y = self.z = 0.0
        self.vx = self.vy = self.vz = 0.0
        self.since = time.monotonic()
        self.presets: Dict[str, Tuple[str, float, float, float]] = {}
        self.home = (0.0, 0.0, 0.0)

    def _integrate(self) -> None:
        now = time.monotonic()
        dt = now - self.since
        self.since = now
        clamp = lambda v, lo, hi: max(lo, min(hi, v))  # noqa: E731
        self.x = clamp(self.x + self.vx * dt * 0.5, -1.0, 1.0)
        self.y = clamp(self.y + self.vy * dt * 0.5, -1.0, 1.0)
        self.z = clamp(self.z + self.vz * dt * 0.5, 0.0, 1.0)

    def position(self) -> Tuple[float, float, float, bool]:
        with self.lock:
            self._integrate()
            return self.x, self.y, self.z, bool(self.vx or self.vy or self.vz)

    def move(self, vx: float, vy: float, vz: float) -> None:
        with self.lock:
            self._integrate()
            self.vx, self.vy, self.vz = vx, vy, vz

    def goto(self, x: float, y: float, z: float) -> None:
        with self.lock:
            self._integrate()
            self.vx = self.vy = self.vz = 0.0
            self.x, self.y, self.z = max(-1, min(1, x)), max(-1, min(1, y)), max(0, min(1, z))


class Camera:
    """State and behaviour of the simulated camera."""

    def __init__(self, settings: Settings, media=None):
        self.s = settings
        self.knobs = Knobs(settings.knobs)
        self.bus = EventBus(self.now)
        self.requests = RequestLog()
        self.digest = DigestAuth("MockCam", settings.username, settings.password)
        self.media = media
        self.enc_lock = threading.Lock()
        self.encoders = copy.deepcopy(DEFAULT_ENCODERS)
        self.ptz = PtzState()
        self.imaging = {"Brightness": 50.0, "ColorSaturation": 50.0, "Contrast": 50.0,
                        "Sharpness": 50.0, "IrCutFilter": "AUTO", "WideDynamicRange": "OFF",
                        "BacklightCompensation": "OFF", "Exposure": "AUTO",
                        "WhiteBalance": "AUTO"}
        self.datetime_type = "NTP"
        self.timezone = "UTC0"
        self.hostname = "mockcam"
        self.started = time.time()
        self.scenario_gen = 0
        self.scenario: Optional[dict] = None
        self.scenario_lock = threading.Lock()
        self.stop = threading.Event()
        self.snapshot_file: Optional[str] = None
        for kind in ("motion_alarm", "cell_motion"):
            self.bus.remember(self.bus.build(kind, False))
        self.bus.remember(self.bus.build("digital_input", False))
        self.bus.remember(self.bus.build("relay", False))
        self._routes = self._build_routes()

    # -- clock and URLs ----------------------------------------------------------

    def now(self) -> float:
        return time.time() + self.knobs["clock_skew_s"]

    def host_for(self, host_header: str) -> Tuple[str, Optional[int]]:
        """(host, port) clients should use: the advertised host, else the Host header."""
        if self.s.advertise_host:
            return self.s.advertise_host, self.s.advertise_http_port
        parts = urlsplit(f"//{host_header or '127.0.0.1'}")
        return parts.hostname or "127.0.0.1", parts.port or 80

    def http_base(self, host_header: str) -> str:
        host, port = self.host_for(host_header)
        return f"http://{host}" + ("" if port in (80, None) else f":{port}")

    def rtsp_uri(self, host_header: str, profile: str) -> str:
        host, _ = self.host_for(host_header)
        port = self.s.advertise_rtsp_port
        return f"rtsp://{host}" + ("" if port == 554 else f":{port}") + f"/{profile}"

    # -- request entry point -----------------------------------------------------

    def handle_soap(self, path: str, headers: Dict[str, str], body: bytes,
                    client: str) -> Response:
        t0 = time.monotonic()
        entry = {"t": iso(time.time()), "client": client, "method": "POST", "path": path,
                 "soap_action": headers.get("soapaction"), "user_agent": headers.get("user-agent"),
                 "content_type": headers.get("content-type")}
        try:
            resp = self._handle(path, headers, body, entry)
        except SoapFault as f:
            entry["fault"] = f.subcode or f.reason
            resp = Response(f.status, f.payload(), dict(f.headers))
        except Exception as e:  # pylint: disable=broad-except
            entry["fault"] = f"internal error: {e}"
            resp = Response(500, fault("Receiver", "ter:Action", f"internal error: {e}"))
        resp.headers.setdefault("Content-Type", "application/soap+xml; charset=utf-8")
        entry["status"] = resp.status
        entry["ms"] = round((time.monotonic() - t0) * 1000, 1)
        if resp.close:
            entry["closed"] = True
        self.requests.add(entry)
        return resp

    def _handle(self, path: str, headers: Dict[str, str], body: bytes, entry: dict) -> Response:
        try:
            req = parse_request(body)
        except SoapParseError as e:
            entry["op"] = "?"
            # Clients that do HTTP Digest by sending an empty first request (curl --digest)
            # expect the challenge, not a complaint about the empty body.
            authz = headers.get("authorization")
            if (self.knobs["auth_mode"] in ("wsse", "digest")
                    and not (authz and self.digest.check(authz, "POST")[0])):
                entry["auth"] = "missing"
                raise SoapFault(401, "Sender", "ter:NotAuthorized", "Sender not Authorized",
                                headers={"WWW-Authenticate": self.digest.challenge()}) from e
            raise SoapFault(400, "Sender", "ter:WellFormed", str(e)) from e
        entry.update(op=req.op, ns=req.op_ns, wsa_to=req.wsa_to, wsa_action=req.wsa_action,
                     ref_params=req.subscription_ref, headers=req.header_names)
        route = self._route(req, path)
        service = route[0] if route else ""
        entry["service"] = service
        self._authenticate(req, service, headers, entry)
        injected = self.knobs.take_fault(req.op)
        if injected:
            entry["injected"] = injected
            if injected["delay_s"]:
                time.sleep(injected["delay_s"])
            if injected["close"]:
                return Response(0, b"", close=True)
            if injected["status"]:
                raise SoapFault(injected["status"], injected["code"], injected["subcode"],
                                injected["reason"])
        if route is None:
            raise not_implemented()
        _, fn = route
        ctx = {"host": headers.get("host", ""), "path": path, "client": entry["client"]}
        result = fn(req, ctx)
        if isinstance(result, Response):
            return result
        return Response(200, envelope(result))

    def _route(self, req: SoapRequest, path: str
               ) -> Optional[Tuple[str, Callable[[SoapRequest, dict], object]]]:
        knobs = self.knobs.snapshot()
        services = [s for s in SERVICE_NS if s not in OPTIONAL_SERVICES or knobs[s]]
        if req.op_ns == NS["wsnt"] and knobs["events"]:
            r = self._routes.get(("events", req.op))
            if r:
                return "events", r
        for svc in services:
            if SERVICE_NS[svc] == req.op_ns and (svc, req.op) in self._routes:
                return svc, self._routes[(svc, req.op)]
        # Clients that put the operation in no namespace: use the service the path names.
        if not req.op_ns:
            for svc in services:
                if path == PATHS[svc] and (svc, req.op) in self._routes:
                    return svc, self._routes[(svc, req.op)]
        if (path.startswith(SUBSCRIPTION_PATH) and req.op in SUBSCRIPTION_OPS
                and knobs["events"]):
            return "events", self._routes[("events", req.op)]
        return None

    def _authenticate(self, req: SoapRequest, service: str, headers: Dict[str, str],
                      entry: dict) -> None:
        mode = self.knobs["auth_mode"]
        authz = headers.get("authorization")
        if mode == "none":
            entry["auth"] = "none-required"
            return
        if (service, req.op) in PRE_AUTH and req.token is None and not authz:
            entry["auth"] = "pre-auth"
            return
        challenge = {"WWW-Authenticate": self.digest.challenge()}
        if mode in ("wsse", "digest") and authz and authz.lower().startswith("digest "):
            ok, stale = self.digest.check(authz, "POST")
            entry["auth"] = "http-digest" if ok else "http-digest-failed"
            if ok:
                return
            raise SoapFault(401, "Sender", "ter:NotAuthorized", "Sender not Authorized",
                            headers={"WWW-Authenticate": self.digest.challenge(stale)})
        if mode in ("wsse", "wsse_only") and req.token is not None:
            ok, why = check_username_token(req.token, self.s.username, self.s.password,
                                           self.now(), self.knobs["created_window_s"])
            entry["auth"] = "wsse" if ok else f"wsse-failed: {why}"
            entry["user"] = req.token.username
            if ok:
                return
            raise SoapFault(400, "Sender", "ter:NotAuthorized", "Sender not Authorized")
        entry["auth"] = "missing"
        if mode == "wsse_only":
            raise SoapFault(400, "Sender", "ter:NotAuthorized", "Sender not Authorized")
        raise SoapFault(401, "Sender", "ter:NotAuthorized", "Sender not Authorized",
                        headers=challenge)

    def _build_routes(self) -> Dict[Tuple[str, str], Callable]:
        routes = {}
        for name in dir(self):
            if not name.startswith("op_"):
                continue
            _, svc, op = name.split("_", 2)
            routes[(svc, op)] = getattr(self, name)
        return routes

    # ============================================================================
    # Device service
    # ============================================================================

    def op_device_GetSystemDateAndTime(self, req, ctx):
        t = time.gmtime(self.now())
        stamp = (f"<tt:Time><tt:Hour>{t.tm_hour}</tt:Hour><tt:Minute>{t.tm_min}</tt:Minute>"
                 f"<tt:Second>{t.tm_sec}</tt:Second></tt:Time><tt:Date><tt:Year>{t.tm_year}"
                 f"</tt:Year><tt:Month>{t.tm_mon}</tt:Month><tt:Day>{t.tm_mday}</tt:Day></tt:Date>")
        return ("<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
                f"<tt:DateTimeType>{esc(self.datetime_type)}</tt:DateTimeType>"
                "<tt:DaylightSavings>false</tt:DaylightSavings>"
                f"<tt:TimeZone><tt:TZ>{esc(self.timezone)}</tt:TZ></tt:TimeZone>"
                f"<tt:UTCDateTime>{stamp}</tt:UTCDateTime>"
                f"<tt:LocalDateTime>{stamp}</tt:LocalDateTime>"
                "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>")

    def op_device_SetSystemDateAndTime(self, req, ctx):
        el = req.op_el
        kind = find_text(el, "DateTimeType") or "Manual"
        tz = find_text(el, "TimeZone", "TZ")
        if tz:
            self.timezone = tz
        self.datetime_type = kind
        utc = child(el, "UTCDateTime")
        if kind == "Manual" and utc is not None:
            vals = [int(_num(find_text(utc, *p))) for p in (
                ("Date", "Year"), ("Date", "Month"), ("Date", "Day"),
                ("Time", "Hour"), ("Time", "Minute"), ("Time", "Second"))]
            target = parse_datetime("%04d-%02d-%02dT%02d:%02d:%02dZ" % tuple(vals))
            if target is None:
                raise invalid_arg("invalid UTCDateTime", "ter:InvalidDateTime")
            self.knobs.update({"clock_skew_s": int(round(target - time.time()))})
        return "<tds:SetSystemDateAndTimeResponse/>"

    def _services(self, host: str, include_caps: bool) -> List[Tuple[str, str, str]]:
        knobs = self.knobs.snapshot()
        base = self.http_base(host)
        out = [(NS["tds"], base + PATHS["device"], "")]
        out.append((NS["trt"], base + PATHS["media"], self._media_caps() if include_caps else ""))
        if knobs["media2"]:
            out.append((NS["tr2"], base + PATHS["media2"], ""))
        if knobs["events"]:
            out.append((NS["tev"], base + PATHS["events"],
                        self._event_caps("tev") if include_caps else ""))
        if knobs["deviceio"]:
            out.append((NS["tmd"], base + PATHS["deviceio"], ""))
        if knobs["imaging"]:
            out.append((NS["timg"], base + PATHS["imaging"], ""))
        if knobs["ptz"]:
            out.append((NS["tptz"], base + PATHS["ptz"], ""))
        if knobs["analytics"]:
            out.append((NS["tan"], base + PATHS["analytics"], ""))
        return out

    def op_device_GetServices(self, req, ctx):
        include = _bool(find_text(req.op_el, "IncludeCapability")) or False
        items = []
        for ns, xaddr, caps in self._services(ctx["host"], include):
            cap = f"<tds:Capabilities>{caps}</tds:Capabilities>" if caps else ""
            items.append(f"<tds:Service><tds:Namespace>{ns}</tds:Namespace>"
                         f"<tds:XAddr>{esc(xaddr)}</tds:XAddr>{cap}<tds:Version>"
                         "<tt:Major>2</tt:Major><tt:Minor>60</tt:Minor></tds:Version>"
                         "</tds:Service>")
        return f"<tds:GetServicesResponse>{''.join(items)}</tds:GetServicesResponse>"

    def op_device_GetCapabilities(self, req, ctx):
        k = self.knobs.snapshot()
        b = self.http_base(ctx["host"])
        parts = []
        if k["analytics"]:
            parts.append(f"<tt:Analytics><tt:XAddr>{b}{PATHS['analytics']}</tt:XAddr>"
                         "<tt:RuleSupport>true</tt:RuleSupport>"
                         "<tt:AnalyticsModuleSupport>true</tt:AnalyticsModuleSupport>"
                         "</tt:Analytics>")
        io = (f"<tt:IO><tt:InputConnectors>{1 if k['deviceio'] else 0}</tt:InputConnectors>"
              f"<tt:RelayOutputs>{1 if k['deviceio'] else 0}</tt:RelayOutputs></tt:IO>")
        parts.append(
            f"<tt:Device><tt:XAddr>{b}{PATHS['device']}</tt:XAddr><tt:Network>"
            "<tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
            "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network>"
            "<tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
            "<tt:DiscoveryBye>false</tt:DiscoveryBye><tt:RemoteDiscovery>false"
            "</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup><tt:SystemLogging>"
            "false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
            "<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>60</tt:Minor>"
            f"</tt:SupportedVersions></tt:System>{io}<tt:Security><tt:TLS1.1>false</tt:TLS1.1>"
            "<tt:TLS1.2>false</tt:TLS1.2><tt:OnboardKeyGeneration>false"
            "</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
            "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
            "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken>"
            "</tt:Security></tt:Device>")
        if k["events"]:
            parts.append(f"<tt:Events><tt:XAddr>{b}{PATHS['events']}</tt:XAddr>"
                         "<tt:WSSubscriptionPolicySupport>false</tt:WSSubscriptionPolicySupport>"
                         "<tt:WSPullPointSupport>true</tt:WSPullPointSupport>"
                         "<tt:WSPausableSubscriptionManagerInterfaceSupport>false"
                         "</tt:WSPausableSubscriptionManagerInterfaceSupport></tt:Events>")
        if k["imaging"]:
            parts.append(f"<tt:Imaging><tt:XAddr>{b}{PATHS['imaging']}</tt:XAddr></tt:Imaging>")
        parts.append(f"<tt:Media><tt:XAddr>{b}{PATHS['media']}</tt:XAddr>"
                     "<tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
                     "<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
                     "</tt:StreamingCapabilities></tt:Media>")
        if k["ptz"]:
            parts.append(f"<tt:PTZ><tt:XAddr>{b}{PATHS['ptz']}</tt:XAddr></tt:PTZ>")
        if k["deviceio"]:
            parts.append(f"<tt:Extension><tt:DeviceIO><tt:XAddr>{b}{PATHS['deviceio']}"
                         "</tt:XAddr><tt:VideoSources>1</tt:VideoSources><tt:VideoOutputs>0"
                         "</tt:VideoOutputs><tt:AudioSources>0</tt:AudioSources><tt:AudioOutputs>"
                         "0</tt:AudioOutputs><tt:RelayOutputs>1</tt:RelayOutputs></tt:DeviceIO>"
                         "</tt:Extension>")
        return ("<tds:GetCapabilitiesResponse><tds:Capabilities>"
                f"{''.join(parts)}</tds:Capabilities></tds:GetCapabilitiesResponse>")

    def op_device_GetServiceCapabilities(self, req, ctx):
        return ("<tds:GetServiceCapabilitiesResponse><tds:Capabilities>"
                "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" "
                "DynDNS=\"false\" NTP=\"1\"/><tds:Security UsernameToken=\"true\" "
                "HttpDigest=\"true\"/><tds:System DiscoveryResolve=\"false\" "
                "DiscoveryBye=\"false\" RemoteDiscovery=\"false\" SystemBackup=\"false\" "
                "SystemLogging=\"false\" FirmwareUpgrade=\"false\"/>"
                "</tds:Capabilities></tds:GetServiceCapabilitiesResponse>")

    def op_device_GetDeviceInformation(self, req, ctx):
        s = self.s
        return ("<tds:GetDeviceInformationResponse>"
                f"<tds:Manufacturer>{esc(s.manufacturer)}</tds:Manufacturer>"
                f"<tds:Model>{esc(s.model)}</tds:Model>"
                f"<tds:FirmwareVersion>{esc(s.firmware)}</tds:FirmwareVersion>"
                f"<tds:SerialNumber>{esc(s.serial)}</tds:SerialNumber>"
                f"<tds:HardwareId>{esc(s.hardware_id)}</tds:HardwareId>"
                "</tds:GetDeviceInformationResponse>")

    def op_device_GetNetworkInterfaces(self, req, ctx):
        host, _ = self.host_for(ctx["host"])
        ipv4 = ""
        if host.replace(".", "").isdigit():
            ipv4 = ("<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:Manual>"
                    f"<tt:Address>{esc(host)}</tt:Address><tt:PrefixLength>24</tt:PrefixLength>"
                    "</tt:Manual><tt:DHCP>false</tt:DHCP></tt:Config></tt:IPv4>")
        return ("<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"eth0\">"
                "<tt:Enabled>true</tt:Enabled><tt:Info><tt:Name>eth0</tt:Name>"
                f"<tt:HwAddress>{esc(self.s.mac)}</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info>"
                f"{ipv4}</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>")

    def op_device_GetScopes(self, req, ctx):
        scopes = ["onvif://www.onvif.org/type/video_encoder",
                  "onvif://www.onvif.org/type/Network_Video_Transmitter",
                  "onvif://www.onvif.org/Profile/Streaming", "onvif://www.onvif.org/Profile/T",
                  f"onvif://www.onvif.org/hardware/{self.s.model}",
                  f"onvif://www.onvif.org/name/{self.s.manufacturer}",
                  "onvif://www.onvif.org/location/homelab"]
        items = "".join("<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
                        f"<tt:ScopeItem>{esc(s)}</tt:ScopeItem></tds:Scopes>" for s in scopes)
        return f"<tds:GetScopesResponse>{items}</tds:GetScopesResponse>"

    def op_device_GetHostname(self, req, ctx):
        return ("<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false"
                f"</tt:FromDHCP><tt:Name>{self.hostname}</tt:Name></tds:HostnameInformation>"
                "</tds:GetHostnameResponse>")

    def op_device_GetNetworkProtocols(self, req, ctx):
        return ("<tds:GetNetworkProtocolsResponse>"
                "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled>"
                f"<tt:Port>{self.s.advertise_http_port}</tt:Port></tds:NetworkProtocols>"
                "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled>"
                f"<tt:Port>{self.s.advertise_rtsp_port}</tt:Port></tds:NetworkProtocols>"
                "</tds:GetNetworkProtocolsResponse>")

    def op_device_GetNTP(self, req, ctx):
        return ("<tds:GetNTPResponse><tds:NTPInformation><tt:FromDHCP>false</tt:FromDHCP>"
                "</tds:NTPInformation></tds:GetNTPResponse>")

    def op_device_GetUsers(self, req, ctx):
        return (f"<tds:GetUsersResponse><tds:User><tt:Username>{esc(self.s.username)}"
                "</tt:Username><tt:UserLevel>Administrator</tt:UserLevel></tds:User>"
                "</tds:GetUsersResponse>")

    def op_device_GetWsdlUrl(self, req, ctx):
        return ("<tds:GetWsdlUrlResponse><tds:WsdlUrl>http://www.onvif.org/</tds:WsdlUrl>"
                "</tds:GetWsdlUrlResponse>")

    def op_device_GetRelayOutputs(self, req, ctx):
        if not self.knobs["deviceio"]:
            return "<tds:GetRelayOutputsResponse/>"
        return (f"<tds:GetRelayOutputsResponse>{self._relay_xml('tds')}"
                "</tds:GetRelayOutputsResponse>")

    def op_device_SetRelayOutputState(self, req, ctx):
        self._set_relay(req)
        return "<tds:SetRelayOutputStateResponse/>"

    def op_device_SystemReboot(self, req, ctx):
        self.bus.drop_all("reboot")
        if self.media:
            self.media.restart_publisher("reboot")
        return ("<tds:SystemRebootResponse><tds:Message>Rebooting (simulated: subscriptions "
                "dropped, streams restarted)</tds:Message></tds:SystemRebootResponse>")

    # ============================================================================
    # Media (ver10)
    # ============================================================================

    def _encoders(self) -> Dict[str, dict]:
        with self.enc_lock:
            return copy.deepcopy(self.encoders)

    def _profile_token(self, req, allow_config=False) -> str:
        tok = find_text(req.op_el, "ProfileToken")
        encs = self._encoders()
        if tok in encs:
            return tok
        if allow_config:
            ctok = find_text(req.op_el, "ConfigurationToken")
            for p, e in encs.items():
                if e["token"] == ctok:
                    return p
            if tok is None and ctok is None:
                return "stream1"
        raise invalid_arg(f"no profile {tok!r}", "ter:NoProfile")

    def _media_caps(self) -> str:
        return ("<trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" "
                "VideoSourceMode=\"false\" OSD=\"false\"><trt:ProfileCapabilities "
                "MaximumNumberOfProfiles=\"2\"/><trt:StreamingCapabilities RTPMulticast=\"false\" "
                "RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" NonAggregateControl=\"false\"/>"
                "</trt:Capabilities>")

    @staticmethod
    def _vsc(prefix: str = "tt", tag: str = "VideoSourceConfiguration") -> str:
        return (f"<{prefix}:{tag} token=\"VSC_1\"><tt:Name>VideoSourceConfig</tt:Name>"
                "<tt:UseCount>2</tt:UseCount><tt:SourceToken>VideoSource_1</tt:SourceToken>"
                f"<tt:Bounds x=\"0\" y=\"0\" width=\"1280\" height=\"720\"/></{prefix}:{tag}>")

    @staticmethod
    def _vec1(e: dict, tag: str = "tt:VideoEncoderConfiguration") -> str:
        return (f"<{tag} token=\"{e['token']}\"><tt:Name>{esc(e['name'])}</tt:Name>"
                "<tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>"
                f"<tt:Resolution><tt:Width>{e['width']}</tt:Width><tt:Height>{e['height']}"
                f"</tt:Height></tt:Resolution><tt:Quality>{e['quality']}</tt:Quality>"
                f"<tt:RateControl><tt:FrameRateLimit>{e['fps']}</tt:FrameRateLimit>"
                "<tt:EncodingInterval>1</tt:EncodingInterval>"
                f"<tt:BitrateLimit>{e['bitrate']}</tt:BitrateLimit></tt:RateControl>"
                f"<tt:H264><tt:GovLength>{e['gov']}</tt:GovLength><tt:H264Profile>"
                f"{esc(e['profile'])}</tt:H264Profile></tt:H264><tt:Multicast><tt:Address>"
                "<tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>"
                "<tt:Port>0</tt:Port>"
                "<tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
                f"<tt:SessionTimeout>PT60S</tt:SessionTimeout></{tag}>")

    @staticmethod
    def _aec1(tag: str = "tt:AudioEncoderConfiguration") -> str:
        return (f"<{tag} token=\"AEC_1\"><tt:Name>AudioEncoder</tt:Name><tt:UseCount>2"
                "</tt:UseCount><tt:Encoding>AAC</tt:Encoding><tt:Bitrate>64</tt:Bitrate>"
                "<tt:SampleRate>16</tt:SampleRate><tt:Multicast><tt:Address><tt:Type>IPv4"
                "</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0"
                "</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
                f"<tt:SessionTimeout>PT60S</tt:SessionTimeout></{tag}>")

    def _vac(self, tag: str = "tt:VideoAnalyticsConfiguration") -> str:
        sens = self.knobs["analytics_sensitivity"]
        return (f"<{tag} token=\"VAC_1\"><tt:Name>VideoAnalytics</tt:Name><tt:UseCount>2"
                "</tt:UseCount><tt:AnalyticsEngineConfiguration><tt:AnalyticsModule "
                "Name=\"MyCellMotionModule\" Type=\"tt:CellMotionEngine\"><tt:Parameters>"
                f"<tt:SimpleItem Name=\"Sensitivity\" Value=\"{sens}\"/></tt:Parameters>"
                "</tt:AnalyticsModule></tt:AnalyticsEngineConfiguration>"
                "<tt:RuleEngineConfiguration>"
                "<tt:Rule Name=\"MyMotionDetectorRule\" Type=\"tt:CellMotionDetector\">"
                "<tt:Parameters><tt:SimpleItem Name=\"MinCount\" Value=\"5\"/></tt:Parameters>"
                f"</tt:Rule></tt:RuleEngineConfiguration></{tag}>")

    @staticmethod
    def _ptzc(tag: str = "tt:PTZConfiguration") -> str:
        return (f"<{tag} token=\"PTZ_1\"><tt:Name>PTZ</tt:Name><tt:UseCount>2</tt:UseCount>"
                "<tt:NodeToken>PTZNode_1</tt:NodeToken><tt:DefaultContinuousPanTiltVelocitySpace>"
                "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"
                "</tt:DefaultContinuousPanTiltVelocitySpace><tt:DefaultPTZTimeout>PT5S"
                f"</tt:DefaultPTZTimeout></{tag}>")

    def _profile1(self, name: str, e: dict) -> str:
        k = self.knobs.snapshot()
        parts = [f"<tt:Name>{esc(e['name'])}</tt:Name>", self._vsc()]
        if k["audio"]:
            parts.append("<tt:AudioSourceConfiguration token=\"ASC_1\"><tt:Name>AudioSource"
                         "</tt:Name><tt:UseCount>2</tt:UseCount><tt:SourceToken>AudioSource_1"
                         "</tt:SourceToken></tt:AudioSourceConfiguration>")
        parts.append(self._vec1(e))
        if k["audio"]:
            parts.append(self._aec1())
        if k["analytics"]:
            parts.append(self._vac())
        if k["ptz"]:
            parts.append(self._ptzc())
        return f"<trt:Profiles token=\"{name}\" fixed=\"true\">{''.join(parts)}</trt:Profiles>"

    def op_media_GetProfiles(self, req, ctx):
        profiles = "".join(self._profile1(n, e) for n, e in self._encoders().items())
        return f"<trt:GetProfilesResponse>{profiles}</trt:GetProfilesResponse>"

    def op_media_GetProfile(self, req, ctx):
        p = self._profile_token(req)
        xml = self._profile1(p, self._encoders()[p]).replace("trt:Profiles", "trt:Profile")
        return f"<trt:GetProfileResponse>{xml}</trt:GetProfileResponse>"

    def op_media_GetStreamUri(self, req, ctx):
        p = self._profile_token(req)
        return ("<trt:GetStreamUriResponse><trt:MediaUri>"
                f"<tt:Uri>{esc(self.rtsp_uri(ctx['host'], p))}</tt:Uri>"
                "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
                "<tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>")

    def op_media_GetSnapshotUri(self, req, ctx):
        self._profile_token(req)
        return ("<trt:GetSnapshotUriResponse><trt:MediaUri>"
                f"<tt:Uri>{esc(self.http_base(ctx['host']) + SNAPSHOT_PATH)}</tt:Uri>"
                "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
                "<tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetSnapshotUriResponse>")

    def op_media_GetVideoEncoderConfigurations(self, req, ctx):
        items = "".join(self._vec1(e, "trt:Configurations") for e in self._encoders().values())
        return (f"<trt:GetVideoEncoderConfigurationsResponse>{items}"
                "</trt:GetVideoEncoderConfigurationsResponse>")

    def op_media_GetVideoEncoderConfiguration(self, req, ctx):
        p = self._profile_token(req, allow_config=True)
        return ("<trt:GetVideoEncoderConfigurationResponse>"
                f"{self._vec1(self._encoders()[p], 'trt:Configuration')}"
                "</trt:GetVideoEncoderConfigurationResponse>")

    def op_media_GetCompatibleVideoEncoderConfigurations(self, req, ctx):
        p = self._profile_token(req)
        return ("<trt:GetCompatibleVideoEncoderConfigurationsResponse>"
                f"{self._vec1(self._encoders()[p], 'trt:Configurations')}"
                "</trt:GetCompatibleVideoEncoderConfigurationsResponse>")

    def _resolutions_xml(self, e: dict) -> str:
        return "".join(f"<tt:ResolutionsAvailable><tt:Width>{w}</tt:Width><tt:Height>{h}"
                       "</tt:Height></tt:ResolutionsAvailable>" for w, h in e["resolutions"])

    def op_media_GetVideoEncoderConfigurationOptions(self, req, ctx):
        e = self._encoders()[self._profile_token(req, allow_config=True)]
        h264 = (f"{self._resolutions_xml(e)}<tt:GovLengthRange><tt:Min>{GOV_RANGE[0]}</tt:Min>"
                f"<tt:Max>{GOV_RANGE[1]}</tt:Max></tt:GovLengthRange><tt:FrameRateRange>"
                f"<tt:Min>{FPS_RANGE[0]}</tt:Min><tt:Max>{FPS_RANGE[1]}</tt:Max>"
                "</tt:FrameRateRange><tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1"
                "</tt:Max></tt:EncodingIntervalRange><tt:H264ProfilesSupported>Baseline"
                "</tt:H264ProfilesSupported><tt:H264ProfilesSupported>Main"
                "</tt:H264ProfilesSupported><tt:H264ProfilesSupported>High"
                "</tt:H264ProfilesSupported>")
        return ("<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options>"
                f"<tt:QualityRange><tt:Min>{QUALITY_RANGE[0]}</tt:Min><tt:Max>{QUALITY_RANGE[1]}"
                f"</tt:Max></tt:QualityRange><tt:H264>{h264}</tt:H264><tt:Extension><tt:H264>"
                f"{h264}<tt:BitrateRange><tt:Min>{BITRATE_RANGE[0]}</tt:Min><tt:Max>"
                f"{BITRATE_RANGE[1]}</tt:Max></tt:BitrateRange></tt:H264></tt:Extension>"
                "</trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>")

    def _apply_encoder(self, conf: Optional[ET.Element], media2: bool) -> None:
        if conf is None:
            raise invalid_arg("missing Configuration")
        if self.knobs["encoder_set"] == "fault":
            raise SoapFault(400, "Sender", "ter:InvalidArgVal", "Configuration cannot be modified",
                            "ter:ConfigModify")
        token = conf.get("token")
        with self.enc_lock:
            name = next((p for p, e in self.encoders.items() if e["token"] == token), None)
            if name is None:
                raise invalid_arg(f"no encoder configuration {token!r}", "ter:NoConfig")
            e = dict(self.encoders[name])
            enc = (find_text(conf, "Encoding") or "H264").upper()
            if enc not in ("H264",):
                raise invalid_arg(f"encoding {enc} not supported", "ter:ConfigModify")
            w = int(_num(find_text(conf, "Resolution", "Width"), e["width"]))
            h = int(_num(find_text(conf, "Resolution", "Height"), e["height"]))
            if (w, h) not in [tuple(r) for r in e["resolutions"]]:
                raise invalid_arg(f"resolution {w}x{h} not offered", "ter:ConfigModify")
            fps = int(_num(find_text(conf, "RateControl", "FrameRateLimit"), e["fps"]))
            rate = int(_num(find_text(conf, "RateControl", "BitrateLimit"), e["bitrate"]))
            q = _num(find_text(conf, "Quality"), e["quality"])
            if media2:
                gov = int(_num(conf.get("GovLength"), e["gov"]))
                prof = conf.get("Profile") or e["profile"]
            else:
                gov = int(_num(find_text(conf, "H264", "GovLength"), e["gov"]))
                prof = find_text(conf, "H264", "H264Profile") or e["profile"]
            if not (FPS_RANGE[0] <= fps <= FPS_RANGE[1] and GOV_RANGE[0] <= gov <= GOV_RANGE[1]
                    and BITRATE_RANGE[0] <= rate <= BITRATE_RANGE[1]):
                raise invalid_arg("frame rate, GOV length or bitrate out of range",
                                  "ter:ConfigModify")
            # The profile goes to x264 as -profile:v, so only the offered ones are accepted.
            canonical = {p.lower(): p for p in H264_PROFILES}.get(prof.strip().lower())
            if canonical is None:
                raise invalid_arg(f"H.264 profile {prof!r} not offered", "ter:ConfigModify")
            prof = canonical
            e.update(width=w, height=h, fps=fps, bitrate=rate, gov=gov,
                     quality=int(q) if q == int(q) else q, profile=prof,
                     name=find_text(conf, "Name") or e["name"])
            changed = e != self.encoders[name]
            self.encoders[name] = e
        if changed and self.media:
            self.media.restart_publisher("encoder configuration changed")

    def op_media_SetVideoEncoderConfiguration(self, req, ctx):
        self._apply_encoder(child(req.op_el, "Configuration"), media2=False)
        return "<trt:SetVideoEncoderConfigurationResponse/>"

    def _video_source(self, tag: str) -> str:
        e = self._encoders()["stream1"]
        return (f"<{tag} token=\"VideoSource_1\"><tt:Framerate>{e['fps']}</tt:Framerate>"
                "<tt:Resolution><tt:Width>1280</tt:Width><tt:Height>720</tt:Height>"
                f"</tt:Resolution></{tag}>")

    def op_media_GetVideoSources(self, req, ctx):
        return (f"<trt:GetVideoSourcesResponse>{self._video_source('trt:VideoSources')}"
                "</trt:GetVideoSourcesResponse>")

    def op_media_GetVideoSourceConfigurations(self, req, ctx):
        return (f"<trt:GetVideoSourceConfigurationsResponse>"
                f"{self._vsc('trt', 'Configurations')}</trt:GetVideoSourceConfigurationsResponse>")

    def op_media_GetVideoSourceConfiguration(self, req, ctx):
        return (f"<trt:GetVideoSourceConfigurationResponse>"
                f"{self._vsc('trt', 'Configuration')}</trt:GetVideoSourceConfigurationResponse>")

    def op_media_GetVideoAnalyticsConfigurations(self, req, ctx):
        inner = self._vac("trt:Configurations") if self.knobs["analytics"] else ""
        return (f"<trt:GetVideoAnalyticsConfigurationsResponse>{inner}"
                "</trt:GetVideoAnalyticsConfigurationsResponse>")

    def op_media_GetAudioEncoderConfigurations(self, req, ctx):
        inner = self._aec1("trt:Configurations") if self.knobs["audio"] else ""
        return (f"<trt:GetAudioEncoderConfigurationsResponse>{inner}"
                "</trt:GetAudioEncoderConfigurationsResponse>")

    def op_media_GetAudioSources(self, req, ctx):
        inner = ("<trt:AudioSources token=\"AudioSource_1\"><tt:Channels>1</tt:Channels>"
                 "</trt:AudioSources>") if self.knobs["audio"] else ""
        return f"<trt:GetAudioSourcesResponse>{inner}</trt:GetAudioSourcesResponse>"

    def op_media_GetMetadataConfigurations(self, req, ctx):
        return "<trt:GetMetadataConfigurationsResponse/>"

    def op_media_GetServiceCapabilities(self, req, ctx):
        return (f"<trt:GetServiceCapabilitiesResponse>{self._media_caps()}"
                "</trt:GetServiceCapabilitiesResponse>")

    # ============================================================================
    # Media2 (ver20)
    # ============================================================================

    @staticmethod
    def _vec2(e: dict, tag: str) -> str:
        return (f"<{tag} token=\"{e['token']}\" GovLength=\"{e['gov']}\" "
                f"Profile=\"{esc(e['profile'])}\"><tt:Name>{esc(e['name'])}</tt:Name>"
                "<tt:UseCount>1"
                "</tt:UseCount><tt:Encoding>H264</tt:Encoding><tt:Resolution><tt:Width>"
                f"{e['width']}</tt:Width><tt:Height>{e['height']}</tt:Height></tt:Resolution>"
                f"<tt:RateControl ConstantBitRate=\"false\"><tt:FrameRateLimit>{e['fps']}"
                f"</tt:FrameRateLimit><tt:BitrateLimit>{e['bitrate']}</tt:BitrateLimit>"
                f"</tt:RateControl><tt:Quality>{e['quality']}</tt:Quality></{tag}>")

    def op_media2_GetProfiles(self, req, ctx):
        types = {(t.text or "").strip() for t in children(req.op_el, "Type")}
        want_tok = find_text(req.op_el, "Token")
        allt = "All" in types
        k = self.knobs.snapshot()
        out = []
        for name, e in self._encoders().items():
            if want_tok and want_tok != name:
                continue
            conf = []
            if allt or "VideoSource" in types:
                conf.append(self._vsc("tr2", "VideoSource"))
            if allt or "VideoEncoder" in types:
                conf.append(self._vec2(e, "tr2:VideoEncoder"))
            if k["audio"] and (allt or "AudioEncoder" in types):
                conf.append(self._aec1("tr2:AudioEncoder"))
            if k["analytics"] and (allt or "Analytics" in types):
                conf.append(self._vac("tr2:Analytics"))
            if k["ptz"] and (allt or "PTZ" in types):
                conf.append(self._ptzc("tr2:PTZ"))
            confs = f"<tr2:Configurations>{''.join(conf)}</tr2:Configurations>" if conf else ""
            out.append(f"<tr2:Profiles token=\"{name}\" fixed=\"true\"><tr2:Name>{esc(e['name'])}"
                       f"</tr2:Name>{confs}</tr2:Profiles>")
        return f"<tr2:GetProfilesResponse>{''.join(out)}</tr2:GetProfilesResponse>"

    def op_media2_GetStreamUri(self, req, ctx):
        p = self._profile_token(req)
        return (f"<tr2:GetStreamUriResponse><tr2:Uri>{esc(self.rtsp_uri(ctx['host'], p))}"
                "</tr2:Uri></tr2:GetStreamUriResponse>")

    def op_media2_GetSnapshotUri(self, req, ctx):
        self._profile_token(req)
        return (f"<tr2:GetSnapshotUriResponse><tr2:Uri>"
                f"{esc(self.http_base(ctx['host']) + SNAPSHOT_PATH)}</tr2:Uri>"
                "</tr2:GetSnapshotUriResponse>")

    def _select_encoders(self, req) -> List[dict]:
        encs = self._encoders()
        ptok = find_text(req.op_el, "ProfileToken")
        ctok = find_text(req.op_el, "ConfigurationToken")
        if ptok:
            if ptok not in encs:
                raise invalid_arg(f"no profile {ptok!r}", "ter:NoProfile")
            return [encs[ptok]]
        if ctok:
            sel = [e for e in encs.values() if e["token"] == ctok]
            if not sel:
                raise invalid_arg(f"no configuration {ctok!r}", "ter:NoConfig")
            return sel
        return list(encs.values())

    def op_media2_GetVideoEncoderConfigurations(self, req, ctx):
        items = "".join(self._vec2(e, "tr2:Configurations") for e in self._select_encoders(req))
        return (f"<tr2:GetVideoEncoderConfigurationsResponse>{items}"
                "</tr2:GetVideoEncoderConfigurationsResponse>")

    def op_media2_GetAudioEncoderConfigurations(self, req, ctx):
        inner = self._aec1("tr2:Configurations") if self.knobs["audio"] else ""
        return (f"<tr2:GetAudioEncoderConfigurationsResponse>{inner}"
                "</tr2:GetAudioEncoderConfigurationsResponse>")

    def op_media2_GetVideoSourceConfigurations(self, req, ctx):
        return (f"<tr2:GetVideoSourceConfigurationsResponse>{self._vsc('tr2', 'Configurations')}"
                "</tr2:GetVideoSourceConfigurationsResponse>")

    def op_media2_GetVideoEncoderConfigurationOptions(self, req, ctx):
        out = []
        for e in self._select_encoders(req)[:1]:
            out.append(f"<tr2:Options Encoding=\"H264\" FrameRatesSupported=\"15 12 10 8 5 3 1\" "
                       f"GovLengthRange=\"{GOV_RANGE[0]} {GOV_RANGE[1]}\" ProfilesSupported="
                       "\"Baseline Main High\"><tt:QualityRange><tt:Min>"
                       f"{QUALITY_RANGE[0]}</tt:Min><tt:Max>{QUALITY_RANGE[1]}</tt:Max>"
                       f"</tt:QualityRange>{self._resolutions_xml(e)}<tt:BitrateRange><tt:Min>"
                       f"{BITRATE_RANGE[0]}</tt:Min><tt:Max>{BITRATE_RANGE[1]}</tt:Max>"
                       "</tt:BitrateRange></tr2:Options>")
        return (f"<tr2:GetVideoEncoderConfigurationOptionsResponse>{''.join(out)}"
                "</tr2:GetVideoEncoderConfigurationOptionsResponse>")

    def op_media2_SetVideoEncoderConfiguration(self, req, ctx):
        self._apply_encoder(child(req.op_el, "Configuration"), media2=True)
        return "<tr2:SetVideoEncoderConfigurationResponse/>"

    def op_media2_GetServiceCapabilities(self, req, ctx):
        return ("<tr2:GetServiceCapabilitiesResponse><tr2:Capabilities SnapshotUri=\"true\" "
                "Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">"
                "<tr2:ProfileCapabilities "
                "MaximumNumberOfProfiles=\"2\"/><tr2:StreamingCapabilities RTSPStreaming=\"true\" "
                "RTPMulticast=\"false\" RTP_RTSP_TCP=\"true\"/></tr2:Capabilities>"
                "</tr2:GetServiceCapabilitiesResponse>")

    # ============================================================================
    # Events
    # ============================================================================

    def _event_caps(self, prefix: str) -> str:
        slots = self.knobs["slots"] or 10
        return (f"<{prefix}:Capabilities WSSubscriptionPolicySupport=\"false\" "
                "WSPullPointSupport=\"true\" WSPausableSubscriptionManagerInterfaceSupport="
                f"\"false\" MaxNotificationProducers=\"0\" MaxPullPoints=\"{slots}\" "
                "PersistentNotificationStorage=\"false\"/>")

    def op_events_GetServiceCapabilities(self, req, ctx):
        return (f"<tev:GetServiceCapabilitiesResponse>{self._event_caps('tev')}"
                "</tev:GetServiceCapabilitiesResponse>")

    def topic_kinds(self) -> list:
        k = self.knobs.snapshot()
        return [kd for kd in KINDS.values() if kd.in_topic_set
                and (k["motion_topics"] or not kd.motion) and (k["deviceio"] or not kd.deviceio)]

    def op_events_GetEventProperties(self, req, ctx):
        return ("<tev:GetEventPropertiesResponse><tev:TopicNamespaceLocation>"
                "http://www.onvif.org/onvif/ver10/topics/topicns.xml</tev:TopicNamespaceLocation>"
                f"<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet>{topic_set_xml(self.topic_kinds())}"
                "<wsnt:TopicExpressionDialect>http://www.onvif.org/ver10/tev/topicExpression/"
                "ConcreteSet</wsnt:TopicExpressionDialect><wsnt:TopicExpressionDialect>"
                "http://docs.oasis-open.org/wsn/t-1/TopicExpression/Concrete"
                "</wsnt:TopicExpressionDialect><tev:MessageContentFilterDialect>"
                "http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter"
                "</tev:MessageContentFilterDialect><tev:MessageContentSchemaLocation>"
                "http://www.onvif.org/onvif/ver10/schema/onvif.xsd"
                "</tev:MessageContentSchemaLocation></tev:GetEventPropertiesResponse>")

    def _times(self, sub) -> str:
        now_real = time.time()
        cam = self.now()
        return (f"<wsnt:CurrentTime>{iso(cam)}</wsnt:CurrentTime>"
                f"<wsnt:TerminationTime>{iso(cam + (sub.expires - now_real))}"
                "</wsnt:TerminationTime>")

    def op_events_CreatePullPointSubscription(self, req, ctx):
        k = self.knobs.snapshot()
        if k["notauthorized_on_create"]:
            raise SoapFault(400, "Sender", "ter:NotAuthorized", "Sender not Authorized")
        requested = find_text(req.op_el, "InitialTerminationTime") or ""
        secs = termination_seconds(requested, self.now(), 60.0)
        if secs <= 0:
            raise invalid_arg("InitialTerminationTime is not in the future",
                              "ter:InvalidTerminationTime")
        sub = self.bus.create(ctx["client"], secs, k["ref_params_required"], requested,
                              k["slots"], k["initial_state"])
        if sub is None:
            raise SoapFault(400, "Sender", "ter:BlockedThreadUpperLimit", "",
                            body=DAHUA_SLOT_FAULT)
        base = self.http_base(ctx["host"])
        if sub.ref_mode:
            addr = base + SUBSCRIPTION_PATH
            ref = ("<wsa:ReferenceParameters xmlns:mc=\"urn:mockcam:onvif:ext\">"
                   f"<mc:SubscriptionId xmlns:mc=\"urn:mockcam:onvif:ext\">{sub.id}"
                   "</mc:SubscriptionId></wsa:ReferenceParameters>")
        else:
            addr = f"{base}{SUBSCRIPTION_PATH}/{sub.id}"
            ref = ""
        cur = self.now()
        return ("<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>"
                f"<wsa:Address>{esc(addr)}</wsa:Address>{ref}</tev:SubscriptionReference>"
                f"<wsnt:CurrentTime>{iso(cur)}</wsnt:CurrentTime>"
                f"<wsnt:TerminationTime>{iso(cur + secs)}</wsnt:TerminationTime>"
                "</tev:CreatePullPointSubscriptionResponse>")

    def _subscription(self, req, ctx):
        """The subscription a Pull/Renew/Unsubscribe/SetSynchronizationPoint addresses."""
        path = ctx["path"]
        sid = None
        by_path = path.startswith(SUBSCRIPTION_PATH + "/")
        if by_path:
            sid = path[len(SUBSCRIPTION_PATH) + 1:]
        elif req.subscription_ref:
            sid = req.subscription_ref
        sub = self.bus.get(sid)
        if sub is None:
            if sid is None:
                raise SoapFault(400, "Sender", "ter:InvalidArgs",
                                "ReferenceParameters missing: no subscription identified")
            raise invalid_arg("unknown subscription", "ter:ResourceUnknown")
        if sub.ref_mode and req.subscription_ref != sub.id:
            raise SoapFault(400, "Sender", "ter:InvalidArgs",
                            "ReferenceParameters missing or wrong")
        if req.op in WSA_OPS and self.knobs["wsa_strict"] and not (req.wsa_to and req.wsa_action):
            raise SoapFault(400, "Sender", "ter:InvalidArgs",
                            "WS-Addressing To and Action headers are required")
        return sub

    def op_events_PullMessages(self, req, ctx):
        sub = self._subscription(req, ctx)
        timeout = parse_duration(find_text(req.op_el, "Timeout") or "")
        if timeout is None:
            timeout = 10.0
        limit = int(_num(find_text(req.op_el, "MessageLimit"), 100))
        msgs = self.bus.pull(sub, min(timeout, 120.0), limit, self.knobs["pull_hold"])
        if msgs is None:
            raise invalid_arg("subscription ended", "ter:ResourceUnknown")
        body = "".join(m.xml() for m in msgs)
        return (f"<tev:PullMessagesResponse>{self._times(sub).replace('wsnt:', 'tev:')}"
                f"{body}</tev:PullMessagesResponse>")

    def op_events_Renew(self, req, ctx):
        sub = self._subscription(req, ctx)
        requested = find_text(req.op_el, "TerminationTime")
        secs = termination_seconds(requested, self.now(), 60.0)
        if secs <= 0:
            raise invalid_arg("TerminationTime is not in the future",
                              "ter:InvalidTerminationTime")
        self.bus.renew(sub, secs)
        return f"<wsnt:RenewResponse>{self._times(sub)}</wsnt:RenewResponse>"

    def op_events_Unsubscribe(self, req, ctx):
        sub = self._subscription(req, ctx)
        self.bus.unsubscribe(sub)
        return "<wsnt:UnsubscribeResponse/>"

    def op_events_SetSynchronizationPoint(self, req, ctx):
        sub = self._subscription(req, ctx)
        self.bus.sync_point(sub)
        return "<tev:SetSynchronizationPointResponse/>"

    # ============================================================================
    # DeviceIO
    # ============================================================================

    @staticmethod
    def _relay_xml(prefix: str) -> str:
        return (f"<{prefix}:RelayOutputs token=\"RO_1\"><tt:Properties><tt:Mode>Bistable"
                "</tt:Mode><tt:DelayTime>PT0S</tt:DelayTime><tt:IdleState>open</tt:IdleState>"
                f"</tt:Properties></{prefix}:RelayOutputs>")

    def _set_relay(self, req) -> None:
        if not self.knobs["deviceio"]:
            raise not_implemented()
        tok = find_text(req.op_el, "RelayOutputToken")
        if tok != "RO_1":
            raise invalid_arg(f"no relay output {tok!r}", "ter:RelayToken")
        active = (find_text(req.op_el, "LogicalState") or "").lower() == "active"
        self.emit("relay", state=active)

    def op_deviceio_GetDigitalInputs(self, req, ctx):
        return ("<tmd:GetDigitalInputsResponse><tmd:DigitalInputs token=\"DI_1\" "
                "IdleState=\"open\"/></tmd:GetDigitalInputsResponse>")

    def op_deviceio_GetRelayOutputs(self, req, ctx):
        return (f"<tmd:GetRelayOutputsResponse>{self._relay_xml('tmd')}"
                "</tmd:GetRelayOutputsResponse>")

    def op_deviceio_GetRelayOutputOptions(self, req, ctx):
        return ("<tmd:GetRelayOutputOptionsResponse><tmd:RelayOutputOptions token=\"RO_1\">"
                "<tmd:Mode>Monostable</tmd:Mode><tmd:Mode>Bistable</tmd:Mode>"
                "<tmd:DelayTimes>1 5 10</tmd:DelayTimes><tmd:Discrete>true</tmd:Discrete>"
                "</tmd:RelayOutputOptions></tmd:GetRelayOutputOptionsResponse>")

    def op_deviceio_SetRelayOutputState(self, req, ctx):
        self._set_relay(req)
        return "<tmd:SetRelayOutputStateResponse/>"

    def op_deviceio_SetRelayOutputSettings(self, req, ctx):
        return "<tmd:SetRelayOutputSettingsResponse/>"

    def op_deviceio_GetVideoSources(self, req, ctx):
        return ("<tmd:GetVideoSourcesResponse><tmd:Token>VideoSource_1</tmd:Token>"
                "</tmd:GetVideoSourcesResponse>")

    def op_deviceio_GetServiceCapabilities(self, req, ctx):
        return ("<tmd:GetServiceCapabilitiesResponse><tmd:Capabilities VideoSources=\"1\" "
                "VideoOutputs=\"0\" AudioSources=\"0\" AudioOutputs=\"0\" RelayOutputs=\"1\" "
                "SerialPorts=\"0\" DigitalInputs=\"1\" DigitalInputOptions=\"false\"/>"
                "</tmd:GetServiceCapabilitiesResponse>")

    # ============================================================================
    # PTZ
    # ============================================================================

    def _ptz_vector(self, el, name) -> Tuple[Optional[float], Optional[float], Optional[float]]:
        v = child(el, name)
        pt, z = child(v, "PanTilt"), child(v, "Zoom")
        return (float(pt.get("x", 0)) if pt is not None else None,
                float(pt.get("y", 0)) if pt is not None else None,
                float(z.get("x", 0)) if z is not None else None)

    def op_ptz_GetServiceCapabilities(self, req, ctx):
        return ("<tptz:GetServiceCapabilitiesResponse><tptz:Capabilities EFlip=\"false\" "
                "Reverse=\"false\" GetCompatibleConfigurations=\"false\" MoveStatus=\"true\" "
                "StatusPosition=\"true\"/></tptz:GetServiceCapabilitiesResponse>")

    def op_ptz_GetNodes(self, req, ctx):
        return ("<tptz:GetNodesResponse><tptz:PTZNode token=\"PTZNode_1\" FixedHomePosition="
                "\"false\"><tt:Name>MockPTZ</tt:Name><tt:SupportedPTZSpaces>"
                "<tt:AbsolutePanTiltPositionSpace><tt:URI>http://www.onvif.org/ver10/tptz/"
                "PanTiltSpaces/PositionGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min>"
                "<tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max>"
                "</tt:YRange></tt:AbsolutePanTiltPositionSpace><tt:AbsoluteZoomPositionSpace>"
                "<tt:URI>http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace</tt:URI>"
                "<tt:XRange><tt:Min>0</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
                "</tt:AbsoluteZoomPositionSpace><tt:ContinuousPanTiltVelocitySpace><tt:URI>"
                "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</tt:URI>"
                "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange><tt:YRange><tt:Min>"
                "-1</tt:Min><tt:Max>1</tt:Max></tt:YRange></tt:ContinuousPanTiltVelocitySpace>"
                "<tt:ContinuousZoomVelocitySpace><tt:URI>http://www.onvif.org/ver10/tptz/"
                "ZoomSpaces/VelocityGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1"
                "</tt:Max></tt:XRange></tt:ContinuousZoomVelocitySpace></tt:SupportedPTZSpaces>"
                "<tt:MaximumNumberOfPresets>16</tt:MaximumNumberOfPresets><tt:HomeSupported>true"
                "</tt:HomeSupported></tptz:PTZNode></tptz:GetNodesResponse>")

    def op_ptz_GetConfigurations(self, req, ctx):
        return (f"<tptz:GetConfigurationsResponse>{self._ptzc('tptz:PTZConfiguration')}"
                "</tptz:GetConfigurationsResponse>")

    def op_ptz_GetStatus(self, req, ctx):
        x, y, z, moving = self.ptz.position()
        st = "MOVING" if moving else "IDLE"
        return ("<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position>"
                f"<tt:PanTilt x=\"{x:.4f}\" y=\"{y:.4f}\"/><tt:Zoom x=\"{z:.4f}\"/>"
                f"</tt:Position><tt:MoveStatus><tt:PanTilt>{st}</tt:PanTilt><tt:Zoom>{st}"
                f"</tt:Zoom></tt:MoveStatus><tt:UtcTime>{iso(self.now())}</tt:UtcTime>"
                "</tptz:PTZStatus></tptz:GetStatusResponse>")

    def op_ptz_ContinuousMove(self, req, ctx):
        vx, vy, vz = self._ptz_vector(req.op_el, "Velocity")
        self.ptz.move(vx or 0.0, vy or 0.0, vz or 0.0)
        return "<tptz:ContinuousMoveResponse/>"

    def op_ptz_AbsoluteMove(self, req, ctx):
        x, y, z = self._ptz_vector(req.op_el, "Position")
        cx, cy, cz, _ = self.ptz.position()
        self.ptz.goto(cx if x is None else x, cy if y is None else y, cz if z is None else z)
        return "<tptz:AbsoluteMoveResponse/>"

    def op_ptz_RelativeMove(self, req, ctx):
        dx, dy, dz = self._ptz_vector(req.op_el, "Translation")
        cx, cy, cz, _ = self.ptz.position()
        self.ptz.goto(cx + (dx or 0), cy + (dy or 0), cz + (dz or 0))
        return "<tptz:RelativeMoveResponse/>"

    def op_ptz_Stop(self, req, ctx):
        self.ptz.move(0.0, 0.0, 0.0)
        return "<tptz:StopResponse/>"

    def op_ptz_GetPresets(self, req, ctx):
        with self.ptz.lock:
            presets = dict(self.ptz.presets)
        items = "".join(
            f"<tptz:Preset token=\"{esc(t)}\"><tt:Name>{esc(n)}</tt:Name><tt:PTZPosition>"
            f"<tt:PanTilt x=\"{x:.4f}\" y=\"{y:.4f}\"/><tt:Zoom x=\"{z:.4f}\"/></tt:PTZPosition>"
            "</tptz:Preset>" for t, (n, x, y, z) in presets.items())
        return f"<tptz:GetPresetsResponse>{items}</tptz:GetPresetsResponse>"

    def op_ptz_SetPreset(self, req, ctx):
        x, y, z, _ = self.ptz.position()
        with self.ptz.lock:
            tok = find_text(req.op_el, "PresetToken") or f"P{len(self.ptz.presets) + 1}"
            self.ptz.presets[tok] = (find_text(req.op_el, "PresetName") or tok, x, y, z)
        return (f"<tptz:SetPresetResponse><tptz:PresetToken>{esc(tok)}</tptz:PresetToken>"
                "</tptz:SetPresetResponse>")

    def op_ptz_GotoPreset(self, req, ctx):
        tok = find_text(req.op_el, "PresetToken")
        with self.ptz.lock:
            p = self.ptz.presets.get(tok or "")
        if not p:
            raise invalid_arg(f"no preset {tok!r}", "ter:NoToken")
        self.ptz.goto(p[1], p[2], p[3])
        return "<tptz:GotoPresetResponse/>"

    def op_ptz_RemovePreset(self, req, ctx):
        with self.ptz.lock:
            self.ptz.presets.pop(find_text(req.op_el, "PresetToken") or "", None)
        return "<tptz:RemovePresetResponse/>"

    def op_ptz_GotoHomePosition(self, req, ctx):
        self.ptz.goto(*self.ptz.home)
        return "<tptz:GotoHomePositionResponse/>"

    def op_ptz_SetHomePosition(self, req, ctx):
        x, y, z, _ = self.ptz.position()
        self.ptz.home = (x, y, z)
        return "<tptz:SetHomePositionResponse/>"

    # ============================================================================
    # Imaging
    # ============================================================================

    def _check_source(self, req) -> None:
        tok = find_text(req.op_el, "VideoSourceToken")
        if tok != "VideoSource_1":
            raise invalid_arg(f"no video source {tok!r}", "ter:NoSource")

    def op_imaging_GetServiceCapabilities(self, req, ctx):
        return ("<timg:GetServiceCapabilitiesResponse><timg:Capabilities ImageStabilization="
                "\"false\" Presets=\"false\"/></timg:GetServiceCapabilitiesResponse>")

    def op_imaging_GetOptions(self, req, ctx):
        self._check_source(req)
        rng = lambda n: f"<tt:{n}><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:{n}>"  # noqa: E731
        return ("<timg:GetOptionsResponse><timg:ImagingOptions>"
                "<tt:BacklightCompensation><tt:Mode>OFF</tt:Mode><tt:Mode>ON</tt:Mode>"
                f"</tt:BacklightCompensation>{rng('Brightness')}{rng('ColorSaturation')}"
                f"{rng('Contrast')}<tt:Exposure><tt:Mode>AUTO</tt:Mode><tt:Mode>MANUAL</tt:Mode>"
                "</tt:Exposure><tt:IrCutFilterModes>ON</tt:IrCutFilterModes><tt:IrCutFilterModes>"
                "OFF</tt:IrCutFilterModes><tt:IrCutFilterModes>AUTO</tt:IrCutFilterModes>"
                f"{rng('Sharpness')}<tt:WideDynamicRange><tt:Mode>OFF</tt:Mode><tt:Mode>ON"
                "</tt:Mode></tt:WideDynamicRange><tt:WhiteBalance><tt:Mode>AUTO</tt:Mode>"
                "<tt:Mode>MANUAL</tt:Mode></tt:WhiteBalance></timg:ImagingOptions>"
                "</timg:GetOptionsResponse>")

    def op_imaging_GetImagingSettings(self, req, ctx):
        self._check_source(req)
        i = {k: esc(v) if isinstance(v, str) else v for k, v in self.imaging.items()}
        return ("<timg:GetImagingSettingsResponse><timg:ImagingSettings>"
                f"<tt:BacklightCompensation><tt:Mode>{i['BacklightCompensation']}</tt:Mode>"
                f"</tt:BacklightCompensation><tt:Brightness>{i['Brightness']:g}</tt:Brightness>"
                f"<tt:ColorSaturation>{i['ColorSaturation']:g}</tt:ColorSaturation>"
                f"<tt:Contrast>{i['Contrast']:g}</tt:Contrast><tt:Exposure><tt:Mode>"
                f"{i['Exposure']}</tt:Mode></tt:Exposure><tt:IrCutFilter>{i['IrCutFilter']}"
                f"</tt:IrCutFilter><tt:Sharpness>{i['Sharpness']:g}</tt:Sharpness>"
                f"<tt:WideDynamicRange><tt:Mode>{i['WideDynamicRange']}</tt:Mode>"
                f"</tt:WideDynamicRange><tt:WhiteBalance><tt:Mode>{i['WhiteBalance']}"
                "</tt:Mode></tt:WhiteBalance></timg:ImagingSettings>"
                "</timg:GetImagingSettingsResponse>")

    def op_imaging_SetImagingSettings(self, req, ctx):
        self._check_source(req)
        st = child(req.op_el, "ImagingSettings")
        for n in ("Brightness", "ColorSaturation", "Contrast", "Sharpness"):
            v = find_text(st, n)
            if v:
                self.imaging[n] = max(0.0, min(100.0, _num(v)))
        v = find_text(st, "IrCutFilter")
        if v:
            self.imaging["IrCutFilter"] = v
        for n in ("BacklightCompensation", "Exposure", "WideDynamicRange", "WhiteBalance"):
            v = find_text(st, n, "Mode")
            if v:
                self.imaging[n] = v
        return "<timg:SetImagingSettingsResponse/>"

    def op_imaging_GetStatus(self, req, ctx):
        self._check_source(req)
        return "<timg:GetStatusResponse><timg:Status/></timg:GetStatusResponse>"

    def op_imaging_GetMoveOptions(self, req, ctx):
        self._check_source(req)
        return "<timg:GetMoveOptionsResponse><timg:MoveOptions/></timg:GetMoveOptionsResponse>"

    # ============================================================================
    # Analytics
    # ============================================================================

    def op_analytics_GetServiceCapabilities(self, req, ctx):
        return ("<tan:GetServiceCapabilitiesResponse><tan:Capabilities RuleSupport=\"true\" "
                "AnalyticsModuleSupport=\"true\" CellBasedSceneDescriptionSupported=\"true\"/>"
                "</tan:GetServiceCapabilitiesResponse>")

    def op_analytics_GetAnalyticsModules(self, req, ctx):
        if find_text(req.op_el, "ConfigurationToken") != "VAC_1":
            raise invalid_arg("no such configuration", "ter:NoConfig")
        sens = self.knobs["analytics_sensitivity"]
        return ("<tan:GetAnalyticsModulesResponse><tan:AnalyticsModule Name=\"MyCellMotionModule\" "
                "Type=\"tt:CellMotionEngine\"><tt:Parameters><tt:SimpleItem Name=\"Sensitivity\" "
                f"Value=\"{sens}\"/></tt:Parameters></tan:AnalyticsModule>"
                "</tan:GetAnalyticsModulesResponse>")

    def op_analytics_ModifyAnalyticsModules(self, req, ctx):
        for item in req.op_el.iter():
            if local(item.tag) == "SimpleItem" and item.get("Name") == "Sensitivity":
                self.knobs.update({"analytics_sensitivity": int(_num(item.get("Value"), 50))})
        return "<tan:ModifyAnalyticsModulesResponse/>"

    def op_analytics_GetRules(self, req, ctx):
        return ("<tan:GetRulesResponse><tan:Rule Name=\"MyMotionDetectorRule\" "
                "Type=\"tt:CellMotionDetector\"><tt:Parameters><tt:SimpleItem Name=\"MinCount\" "
                "Value=\"5\"/></tt:Parameters></tan:Rule></tan:GetRulesResponse>")

    def op_analytics_GetSupportedAnalyticsModules(self, req, ctx):
        return ("<tan:GetSupportedAnalyticsModulesResponse><tan:SupportedAnalyticsModules>"
                "<tt:AnalyticsModuleDescription Name=\"tt:CellMotionEngine\"><tt:Parameters>"
                "<tt:SimpleItemDescription Name=\"Sensitivity\" Type=\"xs:integer\"/>"
                "</tt:Parameters><tt:Messages IsProperty=\"true\"><tt:Source/><tt:ParentTopic>"
                "tns1:RuleEngine/CellMotionDetector/Motion</tt:ParentTopic></tt:Messages>"
                "</tt:AnalyticsModuleDescription></tan:SupportedAnalyticsModules>"
                "</tan:GetSupportedAnalyticsModulesResponse>")

    # ============================================================================
    # Control operations (control API)
    # ============================================================================

    def emit(self, kind: str, state: Optional[bool] = True, **opts) -> dict:
        """Publishes one event of @p kind; stateful kinds also update the current state."""
        if kind not in KINDS:
            raise ValueError(f"unknown kind {kind!r}; known: {', '.join(sorted(KINDS))}")
        check_object_opts(opts.get("cls"), opts.get("bbox"))
        n = self.bus.build(kind, state, **opts)
        if KINDS[kind].state_item:
            self.bus.remember(n)
        delivered = self.bus.publish([n])
        return {"subscriptions": delivered, "message": n.summary()}

    def emit_raw(self, xml: str) -> dict:
        decls = " ".join('xmlns:%s="%s"' % (p, u) for p, u in NS.items())
        try:
            ET.fromstring(f"<w {decls}>{xml}</w>")
        except ET.ParseError as e:
            raise ValueError(f"raw_xml is not well-formed with the envelope's prefixes: {e}") from e
        n = Notification(topic="(raw)", utc=self.now(), source=(), data=(), raw_xml=xml)
        return {"subscriptions": self.bus.publish([n]), "message": n.summary()}

    def set_subject(self, subject: str) -> None:
        self.knobs.update({"subject": subject})
        if self.media:
            self.media.set_subject(subject)
