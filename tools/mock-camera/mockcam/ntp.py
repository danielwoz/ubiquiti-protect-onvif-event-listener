"""Minimal SNTP client for the simulated camera clock.

The client queries the configured server every ntp_interval_s seconds and adds the
measured offset to the camera's own clock (GetSystemDateAndTime, event timestamps).
The host clock is never changed: this exists so the simulator can follow the same
NTP server a real camera would use, and so the exchange is visible while debugging.

Wire format is RFC 5905: a 48-byte request with version 4, mode 3 (client), and a
reply in mode 4 (server).  Timestamps are seconds since 1900-01-01 UTC.
"""

import logging
import socket
import struct
import threading
import time
from typing import Any, Dict, Tuple

log = logging.getLogger("mockcam.ntp")

PACKET_SIZE = 48
EPOCH_OFFSET_S = 2208988800   # 1900-01-01 -> 1970-01-01
DEFAULT_PORT = 123


def encode_timestamp(unix_s: float) -> bytes:
    """NTP 64-bit timestamp (seconds, fraction) for a Unix time."""
    ntp = unix_s + EPOCH_OFFSET_S
    sec = int(ntp) & 0xFFFFFFFF
    frac = int((ntp - int(ntp)) * (1 << 32)) & 0xFFFFFFFF
    return struct.pack("!II", sec, frac)


def decode_timestamp(raw: bytes) -> float:
    sec, frac = struct.unpack("!II", raw)
    return sec - EPOCH_OFFSET_S + frac / (1 << 32)


def parse_server(server: str) -> Tuple[str, int]:
    """'host' or 'host:port' -> (host, port).  Raises ValueError on bad input."""
    s = server.strip()
    if not s:
        raise ValueError("empty NTP server")
    if any(c.isspace() for c in s):
        raise ValueError(f"bad NTP server {server!r}; use host or host:port")
    host, port = s, DEFAULT_PORT
    if ":" in s:
        host, _, p = s.rpartition(":")
        if not host or not p.isdigit():
            raise ValueError(f"bad NTP server {server!r}; use host or host:port")
        port = int(p)
        if not 1 <= port <= 65535:
            raise ValueError(f"bad NTP port in {server!r}")
    return host, port


def query(server: str, timeout_s: float = 2.0) -> Dict[str, Any]:
    """One SNTP exchange.  Raises OSError/ValueError when the server does not answer
    with a usable reply."""
    host, port = parse_server(server)
    request = bytearray(PACKET_SIZE)
    request[0] = (4 << 3) | 3          # LI=0, VN=4, Mode=3 (client)
    t1 = time.time()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout_s)
        sock.sendto(bytes(request), (host, port))
        data, _ = sock.recvfrom(1024)
    t4 = time.time()
    if len(data) < PACKET_SIZE:
        raise OSError(f"short NTP reply from {server} ({len(data)} bytes)")
    version = (data[0] >> 3) & 0x07
    mode = data[0] & 0x07
    stratum = data[1]
    if mode not in (4, 5):
        raise OSError(f"unexpected NTP mode {mode} from {server}")
    if not 1 <= version <= 4:
        raise OSError(f"unexpected NTP version {version} from {server}")
    if not 1 <= stratum <= 15:
        raise OSError(f"{server} is not synchronised (stratum {stratum})")
    t2 = decode_timestamp(data[32:40])
    t3 = decode_timestamp(data[40:48])
    offset = ((t2 - t1) + (t3 - t4)) / 2.0
    roundtrip = (t4 - t1) - (t3 - t2)
    ref_id = data[12:16].decode("latin-1")
    return {
        "offset_s": offset,
        "roundtrip_s": roundtrip,
        "stratum": stratum,
        "version": version,
        "ref_id": ref_id,
        "server_time_s": t3,
    }


class NtpClient:
    """Background SNTP client; keeps the last offset and a small status dict."""

    def __init__(self, knobs, stop: threading.Event):
        self._knobs = knobs
        self._stop = stop
        self._wake = threading.Event()
        self._lock = threading.Lock()
        self._offset = 0.0
        self._state: Dict[str, Any] = {
            "server": "",
            "last_sync": None,
            "offset_ms": None,
            "roundtrip_ms": None,
            "stratum": None,
            "version": None,
            "ref_id": None,
            "last_error": None,
        }
        self._thread = threading.Thread(target=self._run, name="mockcam-ntp", daemon=True)
        self._thread.start()

    def offset_s(self) -> float:
        with self._lock:
            return self._offset

    def state(self) -> Dict[str, Any]:
        with self._lock:
            out = dict(self._state)
        server = self._knobs["ntp_server"]
        out["enabled"] = bool(server)
        out["server"] = server
        out["interval_s"] = self._knobs["ntp_interval_s"]
        return out

    def wake(self) -> None:
        """Applies a new server or interval without waiting out the current one."""
        self._wake.set()

    def _run(self) -> None:
        while not self._stop.is_set():
            server = self._knobs["ntp_server"].strip()
            interval = max(1, int(self._knobs["ntp_interval_s"]))
            if not server:
                with self._lock:
                    self._offset = 0.0
                    if self._state["server"] or self._state["last_error"]:
                        self._state.update(server="", last_error=None, last_sync=None,
                                           offset_ms=None, stratum=None, version=None)
                self._stop.wait(0.5)
                continue
            try:
                r = query(server, timeout_s=min(5.0, float(interval)))
            except (OSError, ValueError) as e:
                with self._lock:
                    self._state.update(server=server, last_error=str(e))
                log.warning("[ntp] %s: %s", server, e)
                self._wait(interval)
                continue
            offset_ms = r["offset_s"] * 1000.0
            with self._lock:
                self._offset = r["offset_s"]
                self._state.update(server=server, last_sync=time.strftime(
                    "%Y-%m-%dT%H:%M:%SZ", time.gmtime()), offset_ms=round(offset_ms, 3),
                    roundtrip_ms=round(r["roundtrip_s"] * 1000.0, 3),
                    stratum=r["stratum"], version=r["version"], ref_id=r["ref_id"],
                    last_error=None)
            log.info("[ntp] %s: offset %+.1f ms, round trip %.1f ms, stratum %d (NTPv%d)",
                     server, offset_ms, r["roundtrip_s"] * 1000.0, r["stratum"], r["version"])
            self._wait(interval)

    def _wait(self, seconds: float) -> None:
        end = time.monotonic() + seconds
        while not self._stop.is_set():
            remain = end - time.monotonic()
            if remain <= 0:
                return
            if self._wake.wait(min(remain, 0.5)):
                self._wake.clear()
                return
