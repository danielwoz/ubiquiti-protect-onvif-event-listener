"""SOAP 1.2 envelopes, faults, time formats and the two ONVIF authentication schemes.

Requests are dispatched on the first child element of the SOAP Body, because UniFi
Protect sends neither a SOAPAction header nor an action= Content-Type parameter.
"""

import base64
import calendar
import hashlib
import hmac
import os
import re
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple
from xml.sax.saxutils import escape as _xml_escape

SOAP12 = "http://www.w3.org/2003/05/soap-envelope"
SOAP11 = "http://schemas.xmlsoap.org/soap/envelope/"
WSA_NAMESPACES = ("http://www.w3.org/2005/08/addressing",
                  "http://schemas.xmlsoap.org/ws/2004/08/addressing")
MOCK_NS = "urn:mockcam:onvif:ext"

NS = {
    "s": SOAP12,
    "tt": "http://www.onvif.org/ver10/schema",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tr2": "http://www.onvif.org/ver20/media/wsdl",
    "tev": "http://www.onvif.org/ver10/events/wsdl",
    "wsnt": "http://docs.oasis-open.org/wsn/b-2",
    "wsa": "http://www.w3.org/2005/08/addressing",
    "wstop": "http://docs.oasis-open.org/wsn/t-1",
    "tns1": "http://www.onvif.org/ver10/topics",
    "ter": "http://www.onvif.org/ver10/error",
    "tmd": "http://www.onvif.org/ver10/deviceIO/wsdl",
    "tptz": "http://www.onvif.org/ver20/ptz/wsdl",
    "timg": "http://www.onvif.org/ver20/imaging/wsdl",
    "tan": "http://www.onvif.org/ver20/analytics/wsdl",
    "xs": "http://www.w3.org/2001/XMLSchema",
    "mc": MOCK_NS,
}

_ENVELOPE_OPEN = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<s:Envelope " + " ".join(
    f'xmlns:{p}="{u}"' for p, u in NS.items()) + ">"

CONTENT_TYPE = "application/soap+xml; charset=utf-8"


def esc(value) -> str:
    """XML-escapes text and attribute values."""
    return _xml_escape(str(value), {'"': "&quot;"})


def envelope(body: str, header: str = "") -> bytes:
    """Wraps @p body (and an optional header) in a SOAP 1.2 envelope."""
    head = f"<s:Header>{header}</s:Header>" if header else ""
    return (f"{_ENVELOPE_OPEN}{head}<s:Body>{body}</s:Body></s:Envelope>").encode("utf-8")


def fault_body(code: str, subcode: str, reason: str, nested: str = "") -> str:
    """A SOAP 1.2 Fault element.  @p code is Sender or Receiver."""
    sub = ""
    if subcode:
        inner = f"<s:Subcode><s:Value>{esc(nested)}</s:Value></s:Subcode>" if nested else ""
        sub = f"<s:Subcode><s:Value>{esc(subcode)}</s:Value>{inner}</s:Subcode>"
    return (f"<s:Fault><s:Code><s:Value>s:{esc(code)}</s:Value>{sub}</s:Code>"
            f"<s:Reason><s:Text xml:lang=\"en\">{esc(reason)}</s:Text></s:Reason></s:Fault>")


def fault(code: str, subcode: str, reason: str, nested: str = "") -> bytes:
    return envelope(fault_body(code, subcode, reason, nested))


# The CreatePullPointSubscription refusal a Dahua SD4A425DB returns once its
# subscription slots are used up: HTTP 400, nested ter:BlockedThreadUpperLimit
# subcodes and an empty Reason.
DAHUA_SLOT_FAULT = (
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:ter=\"http://www.onvif.org/ver10/error\">"
    "<SOAP-ENV:Body><SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Sender"
    "</SOAP-ENV:Value><SOAP-ENV:Subcode><SOAP-ENV:Value>ter:BlockedThreadUpperLimit"
    "</SOAP-ENV:Value><SOAP-ENV:Subcode><SOAP-ENV:Value>ter:BlockedThreadUpperLimit"
    "</SOAP-ENV:Value></SOAP-ENV:Subcode></SOAP-ENV:Subcode></SOAP-ENV:Code>"
    "<SOAP-ENV:Reason><SOAP-ENV:Text xml:lang=\"en\"/></SOAP-ENV:Reason></SOAP-ENV:Fault>"
    "</SOAP-ENV:Body></SOAP-ENV:Envelope>").encode("utf-8")


def local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1] if tag.startswith("{") else tag


def ns_of(tag: str) -> str:
    return tag[1:].split("}", 1)[0] if tag.startswith("{") else ""


def child(el: Optional[ET.Element], name: str) -> Optional[ET.Element]:
    """First direct child of @p el with local name @p name, in any namespace."""
    if el is None:
        return None
    for c in el:
        if local(c.tag) == name:
            return c
    return None


def children(el: Optional[ET.Element], name: str) -> List[ET.Element]:
    if el is None:
        return []
    return [c for c in el if local(c.tag) == name]


def find_text(el: Optional[ET.Element], *path: str) -> Optional[str]:
    """Text of the element reached by local names @p path below @p el."""
    cur = el
    for name in path:
        cur = child(cur, name)
        if cur is None:
            return None
    return (cur.text or "").strip()


def descendant(el: Optional[ET.Element], name: str) -> Optional[ET.Element]:
    if el is None:
        return None
    for d in el.iter():
        if local(d.tag) == name:
            return d
    return None


@dataclass
class UsernameToken:
    username: str = ""
    password: str = ""
    password_type: str = ""
    nonce: str = ""
    created: str = ""


@dataclass
class SoapRequest:
    """A parsed SOAP request: the Body operation and the header blocks the camera uses."""
    op: str = ""
    op_ns: str = ""
    op_el: Optional[ET.Element] = None
    token: Optional[UsernameToken] = None
    wsa_to: Optional[str] = None
    wsa_action: Optional[str] = None
    subscription_ref: Optional[str] = None
    header_names: List[str] = field(default_factory=list)


class SoapParseError(Exception):
    pass


def parse_request(body: bytes) -> SoapRequest:
    try:
        root = ET.fromstring(body)
    except ET.ParseError as e:
        raise SoapParseError(f"not well-formed XML: {e}") from e
    if local(root.tag) != "Envelope" or ns_of(root.tag) not in (SOAP12, SOAP11):
        raise SoapParseError("root element is not a SOAP Envelope")
    req = SoapRequest()
    body_el = child(root, "Body")
    if body_el is None or len(body_el) == 0:
        raise SoapParseError("SOAP Body is missing or empty")
    req.op_el = body_el[0]
    req.op = local(req.op_el.tag)
    req.op_ns = ns_of(req.op_el.tag)
    header = child(root, "Header")
    if header is not None:
        for h in header:
            name = local(h.tag)
            req.header_names.append(name)
            hns = ns_of(h.tag)
            if name == "Security":
                ut = descendant(h, "UsernameToken")
                if ut is not None:
                    pw = child(ut, "Password")
                    req.token = UsernameToken(
                        username=find_text(ut, "Username") or "",
                        password=(pw.text or "").strip() if pw is not None else "",
                        password_type=(pw.get("Type") or "") if pw is not None else "",
                        nonce=find_text(ut, "Nonce") or "",
                        created=find_text(ut, "Created") or "")
            elif name == "To" and hns in WSA_NAMESPACES:
                req.wsa_to = (h.text or "").strip()
            elif name == "Action" and hns in WSA_NAMESPACES:
                req.wsa_action = (h.text or "").strip()
            elif name == "SubscriptionId" and hns == MOCK_NS:
                req.subscription_ref = (h.text or "").strip()
    return req


# ----------------------------------------------------------------------------
# Time formats
# ----------------------------------------------------------------------------

def iso(t: float) -> str:
    """xs:dateTime in UTC with second precision, e.g. 2026-10-28T07:00:00Z."""
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


_DATETIME = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(\.\d+)?(Z|[+-]\d{2}:?\d{2})?$")


def parse_datetime(text: str) -> Optional[float]:
    """Parses xs:dateTime with or without fractional seconds; no zone means UTC."""
    m = _DATETIME.match(text.strip())
    if not m:
        return None
    y, mo, d, h, mi, s = (int(m.group(i)) for i in range(1, 7))
    t = calendar.timegm((y, mo, d, h, mi, s, 0, 0, 0))
    if m.group(7):
        t += float(m.group(7))
    zone = m.group(8)
    if zone and zone != "Z":
        sign = 1 if zone[0] == "+" else -1
        hh, mm = int(zone[1:3]), int(zone[-2:])
        t -= sign * (hh * 3600 + mm * 60)
    return t


_DURATION = re.compile(
    r"^P(?:(\d+)Y)?(?:(\d+)M)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?)?$")


def parse_duration(text: str) -> Optional[float]:
    """Seconds in an xs:duration such as PT60S, PT2M or P1DT1H."""
    m = _DURATION.match(text.strip())
    if not m or text.strip() in ("P", "PT"):
        return None
    y, mo, d, h, mi = (int(m.group(i) or 0) for i in range(1, 6))
    s = float(m.group(6) or 0)
    return ((y * 365 + mo * 30 + d) * 24 + h) * 3600 + mi * 60 + s


def termination_seconds(text: Optional[str], now: float, default: float) -> float:
    """Seconds from @p now to the termination a client asked for, duration or dateTime."""
    if not text:
        return default
    d = parse_duration(text)
    if d is not None:
        return d
    t = parse_datetime(text)
    if t is not None:
        return t - now
    return default


# ----------------------------------------------------------------------------
# WS-Security UsernameToken
# ----------------------------------------------------------------------------

PASSWORD_DIGEST = "PasswordDigest"


def check_username_token(tok: UsernameToken, username: str, password: str,
                         camera_now: float, window_s: float) -> Tuple[bool, str]:
    """Verifies a WS-UsernameToken.  Returns (ok, reason when not ok)."""
    if tok.username != username:
        return False, "unknown user"
    if tok.password_type.endswith(PASSWORD_DIGEST):
        try:
            nonce = base64.b64decode(tok.nonce, validate=False)
        except (ValueError, TypeError):
            return False, "bad nonce"
        expect = base64.b64encode(hashlib.sha1(
            nonce + tok.created.encode("utf-8") + password.encode("utf-8")).digest()).decode()
        if not hmac.compare_digest(expect, tok.password):
            return False, "digest mismatch"
    elif not hmac.compare_digest(tok.password.encode("utf-8"), password.encode("utf-8")):
        return False, "password mismatch"
    if window_s > 0:
        created = parse_datetime(tok.created) if tok.created else None
        if created is None:
            return False, "missing or bad Created"
        if abs(created - camera_now) > window_s:
            return False, f"Created {tok.created} outside +/-{window_s:g}s of camera time"
    return True, ""


def make_username_token(username: str, password: str, created: str,
                        nonce: Optional[bytes] = None) -> str:
    """A wsse:Security header block with a PasswordDigest token (used by tests and tools)."""
    nonce = nonce if nonce is not None else os.urandom(16)
    digest = base64.b64encode(hashlib.sha1(
        nonce + created.encode() + password.encode()).digest()).decode()
    return (
        "<wsse:Security xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-wssecurity-secext-1.0.xsd\" xmlns:wsu=\"http://docs.oasis-open.org/"
        "wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken>"
        f"<wsse:Username>{esc(username)}</wsse:Username>"
        "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-"
        f"username-token-profile-1.0#PasswordDigest\">{digest}</wsse:Password>"
        f"<wsse:Nonce>{base64.b64encode(nonce).decode()}</wsse:Nonce>"
        f"<wsu:Created>{esc(created)}</wsu:Created></wsse:UsernameToken></wsse:Security>")


# ----------------------------------------------------------------------------
# HTTP Digest (RFC 7616, MD5, qop=auth) for SOAP fallback and the snapshot
# ----------------------------------------------------------------------------

def _md5(s: str) -> str:
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def parse_auth_params(value: str) -> Dict[str, str]:
    out = {}
    for m in re.finditer(r'(\w+)\s*=\s*(?:"((?:[^"\\]|\\.)*)"|([^\s,]+))', value):
        out[m.group(1).lower()] = m.group(2) if m.group(2) is not None else m.group(3)
    return out


class DigestAuth:
    """Stateless HTTP Digest: nonces carry their issue time and an HMAC."""

    def __init__(self, realm: str, username: str, password: str, nonce_ttl_s: int = 300):
        self.realm = realm
        self.username = username
        self.password = password
        self.nonce_ttl_s = nonce_ttl_s
        self._key = os.urandom(32)
        self.opaque = hashlib.sha1(self._key).hexdigest()[:16]

    def _nonce(self, ts: int) -> str:
        mac = hmac.new(self._key, str(ts).encode(), hashlib.sha256).hexdigest()[:24]
        return f"{ts:x}{mac}"

    def challenge(self, stale: bool = False) -> str:
        value = (f'Digest realm="{self.realm}", qop="auth", algorithm=MD5, '
                 f'nonce="{self._nonce(int(time.time()))}", opaque="{self.opaque}"')
        return value + (", stale=true" if stale else "")

    def check(self, header: Optional[str], method: str) -> Tuple[bool, bool]:
        """Returns (ok, stale) for an Authorization header value."""
        if not header or not header.lower().startswith("digest "):
            return False, False
        p = parse_auth_params(header[7:])
        nonce = p.get("nonce", "")
        try:
            ts = int(nonce[:-24], 16)
        except ValueError:
            return False, False
        if not hmac.compare_digest(self._nonce(ts), nonce):
            return False, False
        if p.get("username") != self.username or p.get("realm") != self.realm:
            return False, False
        ha1 = _md5(f"{self.username}:{self.realm}:{self.password}")
        ha2 = _md5(f"{method}:{p.get('uri', '')}")
        qop = p.get("qop")
        if qop:
            expect = _md5(f"{ha1}:{nonce}:{p.get('nc', '')}:{p.get('cnonce', '')}:{qop}:{ha2}")
        else:
            expect = _md5(f"{ha1}:{nonce}:{ha2}")
        if not hmac.compare_digest(expect, p.get("response", "")):
            return False, False
        if time.time() - ts > self.nonce_ttl_s:
            return False, True
        return True, False

    def check_basic(self, header: Optional[str]) -> bool:
        if not header or not header.lower().startswith("basic "):
            return False
        try:
            user, _, pw = base64.b64decode(header[6:].strip()).decode("utf-8").partition(":")
        except (ValueError, UnicodeDecodeError):
            return False
        return (hmac.compare_digest(user.encode(), self.username.encode())
                and hmac.compare_digest(pw.encode(), self.password.encode()))


def digest_response_header(username: str, password: str, method: str, uri: str,
                           challenge: str) -> str:
    """Builds an Authorization header answering @p challenge (used by tests and tools)."""
    p = parse_auth_params(challenge[7:] if challenge.lower().startswith("digest ") else challenge)
    ha1 = _md5(f"{username}:{p['realm']}:{password}")
    ha2 = _md5(f"{method}:{uri}")
    cnonce, nc = os.urandom(8).hex(), "00000001"
    resp = _md5(f"{ha1}:{p['nonce']}:{nc}:{cnonce}:auth:{ha2}")
    out = (f'Digest username="{username}", realm="{p["realm"]}", nonce="{p["nonce"]}", '
           f'uri="{uri}", response="{resp}", qop=auth, nc={nc}, cnonce="{cnonce}"')
    if "opaque" in p:
        out += f', opaque="{p["opaque"]}"'
    return out
