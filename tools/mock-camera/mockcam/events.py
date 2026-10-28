"""ONVIF event topics, the current-state store and PullPoint subscriptions."""

import collections
import itertools
import secrets
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Deque, Dict, List, Optional, Sequence, Tuple

from .soap import esc, iso

CONCRETE_SET = "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet"

VSC = ("VideoSourceConfigurationToken", "VSC_1")
VAC = ("VideoAnalyticsConfigurationToken", "VAC_1")
SRC = ("Source", "VideoSource_1")


@dataclass(frozen=True)
class Kind:
    """One kind of event the control API can emit."""
    topic: str
    source: Tuple[Tuple[str, str], ...]
    state_item: Optional[str]          # Data item carrying the state; None = momentary
    on: str = "true"
    off: str = "false"
    object_items: bool = False         # ObjectId / ClassTypes / BoundingBox apply
    default_class: str = ""
    in_topic_set: bool = True
    motion: bool = False
    deviceio: bool = False
    source_types: Tuple[str, ...] = ()


def _my_rule(name: str) -> Kind:
    return Kind(f"tns1:RuleEngine/MyRuleDetector/{name}", (("Source", "000"),), "State")


KINDS: Dict[str, Kind] = {
    "motion_alarm": Kind("tns1:VideoSource/MotionAlarm", (SRC,), "State", motion=True),
    "cell_motion": Kind("tns1:RuleEngine/CellMotionDetector/Motion",
                        (VSC, VAC, ("Rule", "MyMotionDetectorRule")), "IsMotion", motion=True),
    # Topic spelling of a firmware bug seen in the field (tt: prefix, no /Motion).
    "cell_motion_bugtopic": Kind("tns1:RuleEngine/tt:CellMotionDetector", (SRC,), "IsMotion",
                                 in_topic_set=False),
    "field": Kind("tns1:RuleEngine/FieldDetector/ObjectsInside",
                  (VSC, VAC, ("Rule", "MyFieldDetector1")), "IsInside", object_items=True,
                  default_class="Human"),
    "line": Kind("tns1:RuleEngine/LineDetector/Crossed",
                 (VSC, VAC, ("Rule", "MyLineDetector1")), None, object_items=True,
                 default_class="Human"),
    "people": _my_rule("PeopleDetect"),
    "vehicle": _my_rule("VehicleDetect"),
    "face": _my_rule("FaceDetect"),
    "dogcat": _my_rule("DogCatDetect"),
    "visitor": _my_rule("Visitor"),
    "package": _my_rule("Package"),
    "human_shape": Kind("tns1:UserAlarm/IVA/HumanShapeDetect", (VSC,), "State"),
    "vehicle_alarm": Kind("tns1:VehicleAlarm/IVB/VehicleDetect", (VSC,), "State"),
    "digital_input": Kind("tns1:Device/Trigger/DigitalInput", (("InputToken", "DI_1"),),
                          "LogicalState", deviceio=True),
    "relay": Kind("tns1:Device/Trigger/Relay", (("RelayToken", "RO_1"),), "LogicalState",
                  on="active", off="inactive", deviceio=True),
    # A housekeeping topic the recorder ignores silently.
    "noise": Kind("tns1:Monitoring/ProcessorUsage", (("Token", "Processor_Usage"),), None),
    # A topic nothing handles: the recorder logs its unhandled-topic warning once.
    "unknown": Kind("tns1:RuleEngine/MockCam/Unsupported", (SRC,), "State", in_topic_set=False),
}

CLASS_TYPES = ("Human", "Vehicle", "Animal", "Face", "LicensePlate")


@dataclass
class Notification:
    """A NotificationMessage, rendered when it is delivered."""
    topic: str
    utc: float
    source: Tuple[Tuple[str, str], ...]
    data: Tuple[Tuple[str, str], ...]
    property_op: Optional[str] = "Changed"
    element_xml: str = ""
    raw_xml: str = ""

    def xml(self) -> str:
        if self.raw_xml:
            return self.raw_xml
        src = "".join(f'<tt:SimpleItem Name="{esc(n)}" Value="{esc(v)}"/>' for n, v in self.source)
        dat = "".join(f'<tt:SimpleItem Name="{esc(n)}" Value="{esc(v)}"/>' for n, v in self.data)
        op = f' PropertyOperation="{esc(self.property_op)}"' if self.property_op else ""
        return (f'<wsnt:NotificationMessage><wsnt:Topic Dialect="{CONCRETE_SET}">'
                f"{esc(self.topic)}</wsnt:Topic><wsnt:Message>"
                f'<tt:Message UtcTime="{iso(self.utc)}"{op}>'
                f"<tt:Source>{src}</tt:Source><tt:Data>{dat}{self.element_xml}</tt:Data>"
                "</tt:Message></wsnt:Message></wsnt:NotificationMessage>")

    def summary(self) -> dict:
        return {"topic": self.topic, "utc": iso(self.utc), "property_op": self.property_op,
                "source": dict(self.source), "data": dict(self.data),
                "has_bbox": "BoundingBox" in self.element_xml, "raw": bool(self.raw_xml)}


def bbox_xml(box: Sequence[float]) -> str:
    left, top, right, bottom = (float(x) for x in box)
    cx, cy = (left + right) / 2, (top + bottom) / 2
    return ('<tt:ElementItem Name="Shape"><tt:Shape>'
            f'<tt:BoundingBox left="{left:.3f}" top="{top:.3f}" right="{right:.3f}" '
            f'bottom="{bottom:.3f}"/><tt:CenterOfGravity x="{cx:.3f}" y="{cy:.3f}"/>'
            "</tt:Shape></tt:ElementItem>")


def topic_set_xml(kinds: Sequence[Kind]) -> str:
    """A wstop:TopicSet listing @p kinds, nested by topic path."""
    tree: Dict[str, dict] = {}
    leaves: Dict[Tuple[str, ...], Kind] = {}
    for k in kinds:
        parts = tuple(k.topic.split(":", 1)[1].split("/"))
        node = tree
        for p in parts:
            node = node.setdefault(p, {})
        leaves[parts] = k

    def desc(k: Kind) -> str:
        src = "".join(f'<tt:SimpleItemDescription Name="{esc(n)}" Type="tt:ReferenceToken"/>'
                      for n, _ in k.source)
        if k.state_item:
            typ = "xs:string" if k.on == "active" else "xs:boolean"
            dat = f'<tt:SimpleItemDescription Name="{k.state_item}" Type="{typ}"/>'
        elif k.object_items:
            dat = '<tt:SimpleItemDescription Name="ObjectId" Type="xs:integer"/>'
        else:
            dat = '<tt:SimpleItemDescription Name="Value" Type="xs:float"/>'
        if k.object_items:
            dat += '<tt:SimpleItemDescription Name="ClassTypes" Type="tt:ClassType"/>'
        prop = "true" if k.state_item else "false"
        return (f'<tt:MessageDescription IsProperty="{prop}"><tt:Source>{src}</tt:Source>'
                f"<tt:Data>{dat}</tt:Data></tt:MessageDescription>")

    def render(node: dict, path: Tuple[str, ...]) -> str:
        out = []
        for name, sub in node.items():
            here = path + (name,)
            tag = f"tns1:{name}" if not path else name
            inner = render(sub, here)
            if here in leaves:
                out.append(f'<{tag} wstop:topic="true">{desc(leaves[here])}{inner}</{tag}>')
            else:
                out.append(f"<{tag}>{inner}</{tag}>")
        return "".join(out)

    return f"<wstop:TopicSet>{render(tree, ())}</wstop:TopicSet>"


_ids = itertools.count(1)


@dataclass
class Subscription:
    id: str
    client: str
    created: float
    expires: float
    ref_mode: bool
    requested: str
    queue: Deque[Notification] = field(default_factory=collections.deque)
    pulls: int = 0
    renews: int = 0
    delivered: int = 0
    dropped: int = 0
    last_request: float = 0.0
    holding: int = 0
    ended: str = ""

    def info(self, now: float) -> dict:
        return {"id": self.id, "client": self.client, "created": iso(self.created),
                "expires_in_s": round(self.expires - now, 1), "ref_params": self.ref_mode,
                "requested_termination": self.requested, "queued": len(self.queue),
                "pulls": self.pulls, "renews": self.renews, "delivered": self.delivered,
                "dropped": self.dropped, "holding_pulls": self.holding,
                "last_request_age_s": round(now - self.last_request, 1) if self.last_request
                else None, "ended": self.ended or None}


class EventBus:
    """Current event states plus the live pull-point subscriptions."""

    MAX_QUEUE = 1000
    MAX_TERMINATION_S = 86400

    def __init__(self, camera_now: Callable[[], float]):
        self.camera_now = camera_now
        self.cond = threading.Condition()
        self.subs: Dict[str, Subscription] = {}
        self.ended: Deque[Subscription] = collections.deque(maxlen=20)
        self.state: "collections.OrderedDict[tuple, Notification]" = collections.OrderedDict()
        self.history: Deque[dict] = collections.deque(maxlen=200)
        self.creates = 0
        self.refused = 0

    # -- state -----------------------------------------------------------------

    def build(self, kind: str, state: Optional[bool] = True, *, cls: Optional[str] = None,
              rule: Optional[str] = None, object_id: Optional[int] = None,
              bbox: Optional[Sequence[float]] = None, token: Optional[str] = None,
              source: Optional[Dict[str, str]] = None, data: Optional[Dict[str, str]] = None,
              topic: Optional[str] = None, property_op: Optional[str] = None,
              value: Optional[float] = None) -> Notification:
        k = KINDS[kind]
        src = dict(k.source)
        if rule is not None and "Rule" in src:
            src["Rule"] = rule
        if token is not None:
            first = next(iter(src))
            src[first] = token
        if source:
            src.update(source)
        dat: Dict[str, str] = {}
        if k.object_items:
            dat["ObjectId"] = str(object_id if object_id is not None else 1)
        if k.state_item:
            dat[k.state_item] = k.on if state else k.off
        elif not k.object_items:
            dat["Value"] = f"{value if value is not None else 12.5:.6f}"
        if k.object_items:
            c = k.default_class if cls is None else cls
            if c:
                dat["ClassTypes"] = c
        if data:
            dat.update(data)
        return Notification(topic=topic or k.topic, utc=self.camera_now(),
                            source=tuple(src.items()), data=tuple(dat.items()),
                            property_op=property_op if property_op is not None else "Changed",
                            element_xml=bbox_xml(bbox) if bbox else "")

    def remember(self, n: Notification) -> None:
        """Stores a stateful message as the current state of its topic and source."""
        with self.cond:
            self.state[(n.topic, n.source)] = n

    def initial(self) -> List[Notification]:
        """The current state of every stateful topic as Initialized messages."""
        now = self.camera_now()
        with self.cond:
            return [Notification(n.topic, now, n.source, n.data, "Initialized", n.element_xml)
                    for n in self.state.values()]

    def states(self) -> List[dict]:
        with self.cond:
            return [n.summary() for n in self.state.values()]

    def publish(self, msgs: List[Notification]) -> int:
        """Queues @p msgs on every live subscription; returns how many received them."""
        with self.cond:
            self._expire_locked()
            for n in msgs:
                self.history.append(dict(n.summary(), subscribers=len(self.subs)))
            for sub in self.subs.values():
                for n in msgs:
                    if len(sub.queue) >= self.MAX_QUEUE:
                        sub.queue.popleft()
                        sub.dropped += 1
                    sub.queue.append(n)
            self.cond.notify_all()
            return len(self.subs)

    # -- subscriptions ----------------------------------------------------------

    def _expire_locked(self) -> None:
        now = time.time()
        for sid in [s.id for s in self.subs.values() if s.expires <= now]:
            self._end_locked(sid, "expired")

    def _end_locked(self, sid: str, why: str) -> None:
        sub = self.subs.pop(sid, None)
        if sub:
            sub.ended = why
            self.ended.append(sub)
            self.cond.notify_all()

    def live_count(self) -> int:
        with self.cond:
            self._expire_locked()
            return len(self.subs)

    def create(self, client: str, seconds: float, ref_mode: bool, requested: str,
               slots: int, initial: bool) -> Optional[Subscription]:
        """A new subscription, or None when all @p slots are in use."""
        with self.cond:
            self._expire_locked()
            self.creates += 1
            if slots and len(self.subs) >= slots:
                self.refused += 1
                return None
            now = time.time()
            sid = f"{next(_ids)}-{secrets.token_hex(4)}"
            sub = Subscription(id=sid, client=client, created=now,
                               expires=now + min(seconds, self.MAX_TERMINATION_S),
                               ref_mode=ref_mode, requested=requested, last_request=now)
            if initial:
                sub.queue.extend(self.initial())
            self.subs[sid] = sub
            return sub

    def get(self, sid: Optional[str]) -> Optional[Subscription]:
        if not sid:
            return None
        with self.cond:
            self._expire_locked()
            sub = self.subs.get(sid)
            if sub:
                sub.last_request = time.time()
            return sub

    def renew(self, sub: Subscription, seconds: float) -> None:
        with self.cond:
            sub.renews += 1
            sub.expires = time.time() + min(seconds, self.MAX_TERMINATION_S)

    def unsubscribe(self, sub: Subscription, why: str = "unsubscribed") -> None:
        with self.cond:
            self._end_locked(sub.id, why)

    def drop_all(self, why: str = "dropped") -> int:
        with self.cond:
            ids = list(self.subs)
            for sid in ids:
                self._end_locked(sid, why)
            return len(ids)

    def sync_point(self, sub: Subscription) -> None:
        msgs = self.initial()
        with self.cond:
            sub.queue.extend(msgs)
            self.cond.notify_all()

    def pull(self, sub: Subscription, timeout_s: float, limit: int, hold: bool,
             batch_s: float = 0.05) -> Optional[List[Notification]]:
        """Messages for @p sub, waiting up to @p timeout_s for the first one when @p hold.

        Returns None when the subscription ended while waiting.
        """
        deadline = time.monotonic() + max(0.0, timeout_s)
        with self.cond:
            sub.pulls += 1
            sub.holding += 1
            try:
                while hold and not sub.queue and sub.id in self.subs:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        break
                    self.cond.wait(min(left, 1.0))
                    self._expire_locked()
                if sub.id not in self.subs:
                    return None
                if sub.queue and batch_s > 0:
                    # Let messages published together (motion + AI topic) leave together.
                    self.cond.wait(batch_s)
                out = []
                while sub.queue and len(out) < max(1, limit):
                    out.append(sub.queue.popleft())
                sub.delivered += len(out)
                return out
            finally:
                sub.holding -= 1

    def info(self) -> dict:
        now = time.time()
        with self.cond:
            self._expire_locked()
            return {"live": [s.info(now) for s in self.subs.values()],
                    "ended": [s.info(now) for s in self.ended],
                    "creates": self.creates, "refused": self.refused}

    def reaper(self, stop: threading.Event) -> None:
        while not stop.wait(2.0):
            with self.cond:
                self._expire_locked()
