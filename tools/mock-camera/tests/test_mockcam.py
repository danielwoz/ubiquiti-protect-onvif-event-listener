"""Tests of the MockCam SOAP handlers, authentication, events and control API.

Run from tools/mock-camera:  python3 -m pytest -q tests
"""

import http.client
import json
import os
import sys
import threading
import time
import xml.etree.ElementTree as ET

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from mockcam import soap  # noqa: E402
from mockcam.config import Settings  # noqa: E402
from mockcam.events import KINDS  # noqa: E402
from mockcam.server import CameraHandler, ControlHandler, Server, build, make_handler  # noqa: E402

USER, PASSWORD = "mockuser", "Secret123"
NS = {"s": soap.SOAP12, "tt": soap.NS["tt"], "tds": soap.NS["tds"], "trt": soap.NS["trt"],
      "tr2": soap.NS["tr2"], "tev": soap.NS["tev"], "wsnt": soap.NS["wsnt"],
      "wsa": soap.NS["wsa"], "tmd": soap.NS["tmd"], "tptz": soap.NS["tptz"],
      "timg": soap.NS["timg"], "tan": soap.NS["tan"]}
DEV, MEDIA, EVENTS = soap.NS["tds"], soap.NS["trt"], soap.NS["tev"]
JPEG = b"\xff\xd8\xff\xe0" + b"\0" * 64 + b"\xff\xd9"


class Cam:
    def __init__(self, tmp_path, advertise=""):
        s = Settings(username=USER, password=PASSWORD, media=False, advertise_host=advertise,
                     run_dir=str(tmp_path))
        self.cam = build(s)
        self.cam.snapshot_file = str(tmp_path / "snap.jpg")
        (tmp_path / "snap.jpg").write_bytes(JPEG)
        self.srv = Server(("127.0.0.1", 0), make_handler(CameraHandler, self.cam))
        self.ctl = Server(("127.0.0.1", 0), make_handler(ControlHandler, self.cam))
        for srv in (self.srv, self.ctl):
            threading.Thread(target=srv.serve_forever, args=(0.05,), daemon=True).start()
        self.port = self.srv.server_address[1]
        self.ctl_port = self.ctl.server_address[1]

    def close(self):
        self.cam.stop.set()
        for srv in (self.srv, self.ctl):
            srv.shutdown()
            srv.server_close()

    def http(self, method, path, body=b"", headers=None, port=None, timeout=30):
        c = http.client.HTTPConnection("127.0.0.1", port or self.port, timeout=timeout)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        c.close()
        return r.status, hdrs, data

    def soap(self, path, op, ns, inner="", auth="wsse", created=None, extra_header="",
             headers=None, password=PASSWORD):
        header = extra_header
        if auth == "wsse":
            created = created or time.strftime("%Y-%m-%dT%H:%M:%S.123Z", time.gmtime())
            header += soap.make_username_token(USER, password, created)
        head = f"<s:Header>{header}</s:Header>" if header else ""
        body = (f'<?xml version="1.0"?><s:Envelope xmlns:s="{soap.SOAP12}">{head}<s:Body>'
                f'<{op} xmlns="{ns}">{inner}</{op}></s:Body></s:Envelope>').encode()
        h = {"Content-Type": "application/soap+xml; charset=utf-8"}
        h.update(headers or {})
        status, hdrs, data = self.http("POST", path, body, h)
        root = ET.fromstring(data) if data.startswith(b"<?xml") else None
        return status, hdrs, root

    def control(self, method, path, obj=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        status, _, data = self.http(method, path, body, {"Content-Type": "application/json"},
                                    port=self.ctl_port)
        return status, json.loads(data) if data.startswith((b"{", b"[")) else data


@pytest.fixture
def cam(tmp_path):
    c = Cam(tmp_path, advertise="192.168.3.216")
    yield c
    c.close()


def body_of(root):
    return root.find("s:Body", NS)[0]


def fault_subcodes(root):
    return [v.text for v in root.iter(f"{{{soap.SOAP12}}}Value")]


# --------------------------------------------------------------------------- authentication

def test_system_date_and_time_needs_no_credentials(cam):
    cam.cam.knobs.update({"clock_skew_s": 3600})
    status, _, root = cam.soap("/onvif/device_service", "GetSystemDateAndTime", DEV, auth=None)
    assert status == 200
    sdt = body_of(root).find("tds:SystemDateAndTime", NS)
    assert sdt.findtext("tt:DateTimeType", namespaces=NS) == "NTP"
    assert sdt.findtext("tt:DaylightSavings", namespaces=NS) == "false"
    assert sdt.findtext("tt:TimeZone/tt:TZ", namespaces=NS) == "UTC0"
    y = int(sdt.findtext("tt:UTCDateTime/tt:Date/tt:Year", namespaces=NS))
    h = int(sdt.findtext("tt:UTCDateTime/tt:Time/tt:Hour", namespaces=NS))
    assert y == time.gmtime(time.time() + 3600).tm_year
    assert h == time.gmtime(time.time() + 3600).tm_hour


def test_missing_credentials_get_digest_challenge(cam):
    status, hdrs, root = cam.soap("/onvif/device_service", "GetDeviceInformation", DEV, auth=None)
    assert status == 401
    assert hdrs["www-authenticate"].startswith("Digest ")
    assert "ter:NotAuthorized" in fault_subcodes(root)


def test_bad_wsse_password_is_400_not_authorized(cam):
    status, _, root = cam.soap("/onvif/device_service", "GetDeviceInformation", DEV,
                               password="wrong")
    assert status == 400
    assert "ter:NotAuthorized" in fault_subcodes(root)
    assert root.findtext(".//s:Reason/s:Text", namespaces=NS) == "Sender not Authorized"


@pytest.mark.parametrize("created", [
    time.strftime("%Y-%m-%dT%H:%M:%S.456Z", time.gmtime()),   # Protect: milliseconds
    time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),       # onvif-recorder: seconds
])
def test_wsse_digest_accepts_both_created_formats(cam, created):
    status, _, root = cam.soap("/onvif/device_service", "GetDeviceInformation", DEV,
                               created=created)
    assert status == 200
    info = body_of(root)
    assert info.findtext("tds:Manufacturer", namespaces=NS) == "MockCam"
    assert info.findtext("tds:Model", namespaces=NS) == "ONVIF-SIM-1"
    assert info.findtext("tds:SerialNumber", namespaces=NS) == "MOCKCAM0001"


def test_created_window(cam):
    old = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 900))
    assert cam.soap("/onvif/device_service", "GetDeviceInformation", DEV, created=old)[0] == 200
    cam.cam.knobs.update({"created_window_s": 300})
    assert cam.soap("/onvif/device_service", "GetDeviceInformation", DEV, created=old)[0] == 400


def _digest_post(cam, path, op, ns):
    status, hdrs, _ = cam.soap(path, op, ns, auth=None)
    assert status == 401
    authz = soap.digest_response_header(USER, PASSWORD, "POST", path, hdrs["www-authenticate"])
    return cam.soap(path, op, ns, auth=None, headers={"Authorization": authz})


def test_http_digest_fallback(cam):
    status, _, root = _digest_post(cam, "/onvif/device_service", "GetDeviceInformation", DEV)
    assert status == 200
    cam.cam.knobs.update({"auth_mode": "digest"})
    # In digest mode a WS-UsernameToken alone is answered with a challenge.
    status, hdrs, _ = cam.soap("/onvif/device_service", "GetDeviceInformation", DEV)
    assert status == 401 and hdrs["www-authenticate"].startswith("Digest ")
    assert _digest_post(cam, "/onvif/device_service", "GetDeviceInformation", DEV)[0] == 200
    bad = soap.digest_response_header(USER, "nope", "POST", "/onvif/device_service",
                                      hdrs["www-authenticate"])
    status, _, _ = cam.soap("/onvif/device_service", "GetDeviceInformation", DEV, auth=None,
                            headers={"Authorization": bad})
    assert status == 401


def test_wsse_only_mode_has_no_challenge(cam):
    cam.cam.knobs.update({"auth_mode": "wsse_only"})
    status, hdrs, _ = cam.soap("/onvif/device_service", "GetDeviceInformation", DEV, auth=None)
    assert status == 400 and "www-authenticate" not in hdrs


# --------------------------------------------------------------------------- device and media

def test_get_services_default_and_knobs(cam):
    def namespaces():
        status, _, root = cam.soap("/onvif/device_service", "GetServices", DEV,
                                   "<IncludeCapability>false</IncludeCapability>")
        assert status == 200
        return {s.findtext("tds:Namespace", namespaces=NS): s.findtext("tds:XAddr", namespaces=NS)
                for s in body_of(root).findall("tds:Service", NS)}

    ns = namespaces()
    assert ns[soap.NS["tds"]] == "http://192.168.3.216/onvif/device_service"
    assert ns[soap.NS["trt"]] == "http://192.168.3.216/onvif/media_service"
    assert ns[soap.NS["tev"]] == "http://192.168.3.216/onvif/event_service"
    assert soap.NS["tmd"] in ns and soap.NS["timg"] in ns
    assert soap.NS["tr2"] not in ns and soap.NS["tptz"] not in ns and soap.NS["tan"] not in ns
    assert not any("alarm" in n.lower() for n in ns)
    cam.cam.knobs.update({"media2": True, "ptz": True, "analytics": True, "deviceio": False})
    ns = namespaces()
    assert ns[soap.NS["tr2"]].endswith("/onvif/media2_service")
    assert ns[soap.NS["tptz"]].endswith("/onvif/ptz_service")
    assert ns[soap.NS["tan"]].endswith("/onvif/analytics_service")
    assert soap.NS["tmd"] not in ns


def test_network_interface_mac(cam):
    status, _, root = cam.soap("/onvif/device_service", "GetNetworkInterfaces", DEV)
    assert status == 200
    ni = body_of(root).find("tds:NetworkInterfaces", NS)
    assert ni.findtext("tt:Enabled", namespaces=NS) == "true"
    assert ni.findtext("tt:Info/tt:HwAddress", namespaces=NS) == "02:4D:4F:43:4B:01"


def test_profiles_media1(cam):
    status, _, root = cam.soap("/onvif/media_service", "GetProfiles", MEDIA)
    assert status == 200
    profiles = body_of(root).findall("trt:Profiles", NS)
    assert [p.get("token") for p in profiles] == ["stream1", "stream2"]
    got = []
    for p in profiles:
        vec = p.find("tt:VideoEncoderConfiguration", NS)
        assert vec.findtext("tt:Encoding", namespaces=NS) == "H264"
        assert vec.findtext("tt:H264/tt:H264Profile", namespaces=NS) == "Main"
        assert vec.findtext("tt:H264/tt:GovLength", namespaces=NS) == "20"
        got.append((int(vec.findtext("tt:Resolution/tt:Width", namespaces=NS)),
                    int(vec.findtext("tt:Resolution/tt:Height", namespaces=NS)),
                    int(vec.findtext("tt:RateControl/tt:FrameRateLimit", namespaces=NS)),
                    int(vec.findtext("tt:RateControl/tt:BitrateLimit", namespaces=NS))))
        assert p.find("tt:AudioEncoderConfiguration", NS) is None
    assert got == [(1280, 720, 10, 800), (848, 480, 10, 250)]


def test_profiles_media2(cam):
    cam.cam.knobs.update({"media2": True})
    status, _, root = cam.soap("/onvif/media2_service", "GetProfiles", soap.NS["tr2"],
                               "<Type>VideoEncoder</Type><Type>AudioEncoder</Type>")
    assert status == 200
    profs = body_of(root).findall("tr2:Profiles", NS)
    assert [p.get("token") for p in profs] == ["stream1", "stream2"]
    ve = profs[1].find("tr2:Configurations/tr2:VideoEncoder", NS)
    assert ve.findtext("tt:Resolution/tt:Width", namespaces=NS) == "848"
    status, _, root = cam.soap("/onvif/media2_service", "GetStreamUri", soap.NS["tr2"],
                               "<Protocol>RTSP</Protocol><ProfileToken>stream2</ProfileToken>")
    assert body_of(root).findtext("tr2:Uri", namespaces=NS) == "rtsp://192.168.3.216/stream2"


def test_stream_and_snapshot_uris(cam):
    setup = ('<StreamSetup><Stream xmlns="http://www.onvif.org/ver10/schema">RTP-Unicast</Stream>'
             '<Transport xmlns="http://www.onvif.org/ver10/schema"><Protocol>RTSP</Protocol>'
             "</Transport></StreamSetup>")
    for tok in ("stream1", "stream2"):
        status, _, root = cam.soap("/onvif/media_service", "GetStreamUri", MEDIA,
                                   f"{setup}<ProfileToken>{tok}</ProfileToken>")
        assert status == 200
        assert body_of(root).findtext("trt:MediaUri/tt:Uri", namespaces=NS) == \
            f"rtsp://192.168.3.216/{tok}"
    status, _, root = cam.soap("/onvif/media_service", "GetSnapshotUri", MEDIA,
                               "<ProfileToken>stream1</ProfileToken>")
    assert body_of(root).findtext("trt:MediaUri/tt:Uri", namespaces=NS) == \
        "http://192.168.3.216/snapshot.jpg"
    status, _, root = cam.soap("/onvif/media_service", "GetStreamUri", MEDIA,
                               f"{setup}<ProfileToken>nope</ProfileToken>")
    assert status == 400


def test_host_header_used_without_advertise_host(tmp_path):
    c = Cam(tmp_path)
    try:
        _, _, root = c.soap("/onvif/media_service", "GetSnapshotUri", MEDIA,
                            "<ProfileToken>stream1</ProfileToken>")
        assert body_of(root).findtext("trt:MediaUri/tt:Uri", namespaces=NS) == \
            f"http://127.0.0.1:{c.port}/snapshot.jpg"
    finally:
        c.close()


def test_encoder_options_and_set(cam):
    status, _, root = cam.soap("/onvif/media_service", "GetVideoEncoderConfigurationOptions",
                               MEDIA, "<ProfileToken>stream2</ProfileToken>")
    assert status == 200
    res = [(r.findtext("tt:Width", namespaces=NS), r.findtext("tt:Height", namespaces=NS))
           for r in body_of(root).findall("trt:Options/tt:H264/tt:ResolutionsAvailable", NS)]
    assert ("848", "480") in res
    conf = ('<Configuration token="VEC_2"><Name xmlns="http://www.onvif.org/ver10/schema">Sub'
            '</Name><Encoding xmlns="http://www.onvif.org/ver10/schema">H264</Encoding>'
            '<Resolution xmlns="http://www.onvif.org/ver10/schema"><Width>640</Width>'
            "<Height>360</Height></Resolution><RateControl xmlns=\"http://www.onvif.org/ver10/"
            'schema"><FrameRateLimit>5</FrameRateLimit><BitrateLimit>200</BitrateLimit>'
            "</RateControl></Configuration><ForcePersistence>true</ForcePersistence>")
    assert cam.soap("/onvif/media_service", "SetVideoEncoderConfiguration", MEDIA, conf)[0] == 200
    e = cam.cam._encoders()["stream2"]
    assert (e["width"], e["height"], e["fps"], e["bitrate"]) == (640, 360, 5, 200)
    cam.cam.knobs.update({"encoder_set": "fault"})
    assert cam.soap("/onvif/media_service", "SetVideoEncoderConfiguration", MEDIA, conf)[0] == 400


def test_unknown_operation_is_action_not_supported(cam):
    status, _, root = cam.soap("/onvif/device_service", "GetDot11Status", DEV)
    assert status == 400
    assert "ter:ActionNotSupported" in fault_subcodes(root)
    assert "Optional Action Not Implemented" in root.findtext(".//s:Reason/s:Text",
                                                              namespaces=NS)
    # A disabled service answers the same way.
    status, _, root = cam.soap("/onvif/ptz_service", "GetStatus", soap.NS["tptz"],
                               "<ProfileToken>stream1</ProfileToken>")
    assert status == 400 and "ter:ActionNotSupported" in fault_subcodes(root)


def test_deviceio_and_relay_event(cam):
    status, _, root = cam.soap("/onvif/deviceio_service", "GetDigitalInputs", soap.NS["tmd"])
    assert body_of(root).find("tmd:DigitalInputs", NS).get("token") == "DI_1"
    status, _, root = cam.soap("/onvif/deviceio_service", "GetRelayOutputOptions",
                               soap.NS["tmd"])
    assert body_of(root).find("tmd:RelayOutputOptions", NS).get("token") == "RO_1"
    sub = _create(cam, initial=False)
    status, _, _ = cam.soap("/onvif/deviceio_service", "SetRelayOutputState", soap.NS["tmd"],
                            "<RelayOutputToken>RO_1</RelayOutputToken>"
                            "<LogicalState>active</LogicalState>")
    assert status == 200
    msgs = _pull(cam, sub)
    assert [(m["topic"], m["data"]) for m in msgs] == [
        ("tns1:Device/Trigger/Relay", {"LogicalState": "active"})]


def test_imaging(cam):
    status, _, root = cam.soap("/onvif/imaging_service", "GetOptions", soap.NS["timg"],
                               "<VideoSourceToken>VideoSource_1</VideoSourceToken>")
    assert status == 200
    opts = body_of(root).find("timg:ImagingOptions", NS)
    assert opts.findtext("tt:Brightness/tt:Max", namespaces=NS) == "100"
    status, _, root = cam.soap("/onvif/media_service", "GetVideoSources", MEDIA)
    assert body_of(root).find("trt:VideoSources", NS).get("token") == "VideoSource_1"


def test_ptz(cam):
    cam.cam.knobs.update({"ptz": True})
    ptz = soap.NS["tptz"]
    status, _, _ = cam.soap("/onvif/ptz_service", "AbsoluteMove", ptz,
                            '<ProfileToken>stream1</ProfileToken><Position><PanTilt xmlns="'
                            'http://www.onvif.org/ver10/schema" x="0.5" y="-0.25"/></Position>')
    assert status == 200
    _, _, root = cam.soap("/onvif/ptz_service", "GetStatus", ptz,
                          "<ProfileToken>stream1</ProfileToken>")
    pt = body_of(root).find("tptz:PTZStatus/tt:Position/tt:PanTilt", NS)
    assert (float(pt.get("x")), float(pt.get("y"))) == (0.5, -0.25)


def test_analytics_sensitivity(cam):
    cam.cam.knobs.update({"analytics": True, "analytics_sensitivity": 0})
    _, _, root = cam.soap("/onvif/media_service", "GetVideoAnalyticsConfigurations", MEDIA)
    assert body_of(root).find("trt:Configurations", NS).get("token") == "VAC_1"
    _, _, root = cam.soap("/onvif/analytics_service", "GetAnalyticsModules", soap.NS["tan"],
                          "<ConfigurationToken>VAC_1</ConfigurationToken>")
    item = body_of(root).find(".//tt:SimpleItem", NS)
    assert (item.get("Name"), item.get("Value")) == ("Sensitivity", "0")


# --------------------------------------------------------------------------- events

def _create(cam, initial=True, seconds="PT60S", extra=""):
    cam.cam.knobs.update({"initial_state": initial})
    status, _, root = cam.soap("/onvif/event_service", "CreatePullPointSubscription", EVENTS,
                               f"<InitialTerminationTime>{seconds}</InitialTerminationTime>",
                               extra_header=extra)
    assert status == 200, root
    return root


def _addr(root):
    return body_of(root).findtext("tev:SubscriptionReference/wsa:Address", namespaces=NS)


def _pull(cam, sub_root, timeout="PT1S", extra="", expect=200):
    path = "/" + _addr(sub_root).split("/", 3)[3]
    status, _, root = cam.soap(path, "PullMessages", EVENTS,
                               f"<Timeout>{timeout}</Timeout><MessageLimit>100</MessageLimit>",
                               extra_header=extra)
    assert status == expect, ET.tostring(root)[:500] if root is not None else status
    if status != 200:
        return root
    out = []
    for nm in body_of(root).findall("wsnt:NotificationMessage", NS):
        msg = nm.find("wsnt:Message/tt:Message", NS)
        out.append({
            "topic": nm.findtext("wsnt:Topic", namespaces=NS),
            "op": msg.get("PropertyOperation"),
            "source": {i.get("Name"): i.get("Value")
                       for i in msg.findall("tt:Source/tt:SimpleItem", NS)},
            "data": {i.get("Name"): i.get("Value")
                     for i in msg.findall("tt:Data/tt:SimpleItem", NS)},
            "bbox": msg.find(".//tt:BoundingBox", NS),
        })
    return out


def test_event_properties_topic_set(cam):
    status, _, root = cam.soap("/onvif/event_service", "GetEventProperties", EVENTS)
    assert status == 200
    b = body_of(root)
    assert b.find("wsnt:TopicExpressionDialect", NS) is not None
    assert b.find("tev:MessageContentFilterDialect", NS) is not None
    ts = b.find("{http://docs.oasis-open.org/wsn/t-1}TopicSet")
    names = {el.tag.rsplit("}", 1)[-1] for el in ts.iter()}
    assert {"VideoSource", "MotionAlarm", "RuleEngine", "CellMotionDetector", "Motion",
            "FieldDetector", "ObjectsInside", "PeopleDetect", "Trigger", "DigitalInput"} <= names
    cam.cam.knobs.update({"motion_topics": False})
    _, _, root = cam.soap("/onvif/event_service", "GetEventProperties", EVENTS)
    names = {el.tag.rsplit("}", 1)[-1] for el in body_of(root).iter()}
    assert "MotionAlarm" not in names and "CellMotionDetector" not in names


def test_create_absolute_address_and_times(cam):
    root = _create(cam)
    b = body_of(root)
    assert _addr(root).startswith("http://192.168.3.216/onvif/subscription/")
    cur = soap.parse_datetime(b.findtext("wsnt:CurrentTime", namespaces=NS))
    term = soap.parse_datetime(b.findtext("wsnt:TerminationTime", namespaces=NS))
    assert term - cur == 60
    msgs = _pull(cam, root)
    assert {m["topic"] for m in msgs} >= {"tns1:VideoSource/MotionAlarm",
                                          "tns1:RuleEngine/CellMotionDetector/Motion"}
    assert all(m["op"] == "Initialized" for m in msgs)


def test_pull_holds_until_timeout_then_wakes_on_event(cam):
    root = _create(cam, initial=False)
    t0 = time.monotonic()
    assert _pull(cam, root, timeout="PT1S") == []
    assert 0.9 <= time.monotonic() - t0 < 3
    threading.Timer(0.5, lambda: cam.cam.emit("cell_motion", True)).start()
    t0 = time.monotonic()
    msgs = _pull(cam, root, timeout="PT10S")
    assert time.monotonic() - t0 < 3
    assert msgs[0]["topic"] == "tns1:RuleEngine/CellMotionDetector/Motion"
    assert msgs[0]["data"] == {"IsMotion": "true"}
    assert msgs[0]["source"]["Rule"] == "MyMotionDetectorRule"


@pytest.mark.parametrize("kind", sorted(KINDS))
def test_every_kind_has_its_exact_topic(cam, kind):
    root = _create(cam, initial=False)
    status, data = cam.control("POST", "/mock/event", {"kind": kind, "state": True})
    assert status == 200, data
    msgs = _pull(cam, root)
    assert len(msgs) == 1
    m = msgs[0]
    assert m["topic"] == KINDS[kind].topic
    assert m["op"] == "Changed"
    k = KINDS[kind]
    if k.state_item:
        assert m["data"][k.state_item] == k.on
    if k.object_items:
        assert m["data"]["ClassTypes"] == "Human" and "Rule" in m["source"]


def test_field_detector_with_class_and_bbox(cam):
    root = _create(cam, initial=False)
    status, _ = cam.control("POST", "/mock/event", {
        "kind": "field", "state": True, "class": "Vehicle", "rule": "Driveway",
        "bbox": [-0.5, -0.5, 0.25, 0.75], "object_id": 7})
    assert status == 200
    m = _pull(cam, root)[0]
    assert m["source"]["Rule"] == "Driveway"
    assert m["data"] == {"ObjectId": "7", "IsInside": "true", "ClassTypes": "Vehicle"}
    assert m["bbox"] is not None and m["bbox"].get("right") == "0.250"


def test_event_duration_reverts(cam):
    root = _create(cam, initial=False)
    cam.control("POST", "/mock/event", {"kind": "people", "duration_s": 0.3})
    msgs = _pull(cam, root, timeout="PT1S")
    if len(msgs) < 2:
        msgs += _pull(cam, root, timeout="PT2S")
    assert [m["data"]["State"] for m in msgs] == ["true", "false"]


def test_sync_point_replays_state(cam):
    root = _create(cam, initial=False)
    cam.cam.emit("motion_alarm", True)
    _pull(cam, root)
    path = "/" + _addr(root).split("/", 3)[3]
    assert cam.soap(path, "SetSynchronizationPoint", EVENTS)[0] == 200
    msgs = _pull(cam, root)
    ma = [m for m in msgs if m["topic"] == "tns1:VideoSource/MotionAlarm"]
    assert ma and ma[0]["op"] == "Initialized" and ma[0]["data"]["State"] == "true"


def test_renew_and_unsubscribe(cam):
    root = _create(cam, seconds="PT20S", initial=False)
    path = "/" + _addr(root).split("/", 3)[3]
    status, _, r = cam.soap(path, "Renew", soap.NS["wsnt"],
                            "<TerminationTime>PT120S</TerminationTime>")
    assert status == 200
    b = body_of(r)
    cur = soap.parse_datetime(b.findtext("wsnt:CurrentTime", namespaces=NS))
    term = soap.parse_datetime(b.findtext("wsnt:TerminationTime", namespaces=NS))
    assert 119 <= term - cur <= 120
    assert cam.soap(path, "Unsubscribe", soap.NS["wsnt"])[0] == 200
    r = _pull(cam, root, expect=400)
    assert "ter:InvalidArgVal" in fault_subcodes(r)


def test_subscription_expires(cam):
    root = _create(cam, seconds="PT1S", initial=False)
    time.sleep(1.3)
    _pull(cam, root, expect=400)
    assert cam.cam.bus.info()["ended"][-1]["ended"] == "expired"


def test_slots_reproduce_dahua_fault(cam):
    cam.cam.knobs.update({"slots": 1})
    _create(cam)
    status, _, root = cam.soap("/onvif/event_service", "CreatePullPointSubscription", EVENTS,
                               "<InitialTerminationTime>PT60S</InitialTerminationTime>")
    assert status == 400
    assert "ter:BlockedThreadUpperLimit" in fault_subcodes(root)


def test_notauthorized_on_create(cam):
    cam.cam.knobs.update({"notauthorized_on_create": True})
    status, _, root = cam.soap("/onvif/event_service", "CreatePullPointSubscription", EVENTS,
                               "<InitialTerminationTime>PT60S</InitialTerminationTime>")
    assert status == 400 and "ter:NotAuthorized" in fault_subcodes(root)


def test_reference_parameters_required(cam):
    cam.cam.knobs.update({"ref_params_required": True})
    root = _create(cam, initial=False)
    assert _addr(root) == "http://192.168.3.216/onvif/subscription"
    ref = body_of(root).find("tev:SubscriptionReference/wsa:ReferenceParameters", NS)
    sid = ref[0].text
    _pull(cam, root, expect=400)
    hdr = (f'<mc:SubscriptionId xmlns:mc="{soap.MOCK_NS}" '
           f'xmlns:s="{soap.SOAP12}" s:mustUnderstand="1">{sid}</mc:SubscriptionId>')
    assert _pull(cam, root, extra=hdr) == []


def test_wsa_strict(cam):
    cam.cam.knobs.update({"wsa_strict": True})
    root = _create(cam, initial=False)
    _pull(cam, root, expect=400)
    wsa = ('<wsa:To xmlns:wsa="http://www.w3.org/2005/08/addressing">' + _addr(root)
           + '</wsa:To><wsa:Action xmlns:wsa="http://www.w3.org/2005/08/addressing">'
           "http://www.onvif.org/ver10/events/wsdl/PullPointSubscription/PullMessagesRequest"
           "</wsa:Action>")
    assert _pull(cam, root, extra=wsa) == []


def test_fault_next_injection(cam):
    cam.control("POST", "/mock/config", {"fault_next": {"op": "GetDeviceInformation",
                                                        "status": 503, "count": 1}})
    assert cam.soap("/onvif/device_service", "GetDeviceInformation", DEV)[0] == 503
    assert cam.soap("/onvif/device_service", "GetDeviceInformation", DEV)[0] == 200


def test_set_system_date_and_time_manual(cam):
    t = time.gmtime(time.time() + 7200)
    inner = ("<DateTimeType>Manual</DateTimeType><DaylightSavings>false</DaylightSavings>"
             "<UTCDateTime><Date xmlns=\"http://www.onvif.org/ver10/schema\">"
             f"<Year>{t.tm_year}</Year><Month>{t.tm_mon}</Month><Day>{t.tm_mday}</Day></Date>"
             "<Time xmlns=\"http://www.onvif.org/ver10/schema\">"
             f"<Hour>{t.tm_hour}</Hour><Minute>{t.tm_min}</Minute><Second>{t.tm_sec}</Second>"
             "</Time></UTCDateTime>")
    assert cam.soap("/onvif/device_service", "SetSystemDateAndTime", DEV, inner)[0] == 200
    assert abs(cam.cam.knobs["clock_skew_s"] - 7200) <= 2


# --------------------------------------------------------------------------- snapshot / control

def test_snapshot_digest(cam):
    status, hdrs, _ = cam.http("GET", "/snapshot.jpg")
    assert status == 401 and hdrs["www-authenticate"].startswith("Digest ")
    authz = soap.digest_response_header(USER, PASSWORD, "GET", "/snapshot.jpg",
                                        hdrs["www-authenticate"])
    status, hdrs, data = cam.http("GET", "/snapshot.jpg", headers={"Authorization": authz})
    assert status == 200 and hdrs["content-type"] == "image/jpeg" and data == JPEG
    cam.cam.knobs.update({"snapshot_auth": "none"})
    assert cam.http("GET", "/snapshot.jpg")[0] == 200


def test_control_api_validation(cam):
    assert cam.control("POST", "/mock/config", {"nope": 1})[0] == 400
    assert cam.control("POST", "/mock/config", {"auth_mode": "basic"})[0] == 400
    assert cam.control("POST", "/mock/event", {"kind": "bogus"})[0] == 400
    assert cam.control("POST", "/mock/event", {"kind": "field", "bbox": [0, 0, 2, 2]})[0] == 400
    status, data = cam.control("POST", "/mock/config", {"slots": 2, "wsa_strict": True})
    assert status == 200 and sorted(data["changed"]) == ["slots", "wsa_strict"]
    status, data = cam.control("GET", "/mock/requests")
    assert status == 200 and isinstance(data["items"], list)
    status, data = cam.control("GET", "/mock/state")
    assert status == 200 and data["identity"]["mac"] == "02:4D:4F:43:4B:01"


def test_request_log_records_op_and_auth(cam):
    cam.soap("/onvif/device_service", "GetSystemDateAndTime", DEV, auth=None)
    cam.soap("/onvif/device_service", "GetDeviceInformation", DEV)
    _, data = cam.control("GET", "/mock/requests")
    ops = [(e["op"], e["auth"], e["status"]) for e in data["items"]]
    assert ("GetSystemDateAndTime", "pre-auth", 200) in ops
    assert ("GetDeviceInformation", "wsse", 200) in ops


def test_scenario_emits_motion_and_restores(cam):
    root = _create(cam, initial=False)
    status, data = cam.control("POST", "/mock/scenario", {"kind": "person", "seconds": 0.3,
                                                          "lead_s": 0, "tail_s": 0, "ai": True})
    assert status == 200 and data["subject"] == "person"
    msgs = []
    deadline = time.time() + 5
    while len(msgs) < 6 and time.time() < deadline:
        msgs += _pull(cam, root, timeout="PT1S")
    topics = [(m["topic"].rsplit("/", 1)[-1], list(m["data"].values())) for m in msgs]
    assert ("Motion", ["true"]) in topics and ("MotionAlarm", ["true"]) in topics
    assert ("ObjectsInside", ["1", "true", "Human"]) in topics
    assert ("Motion", ["false"]) in topics
    time.sleep(0.2)
    assert cam.cam.knobs["subject"] == "none"
