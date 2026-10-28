# MockCam: a simulated third-party ONVIF camera

MockCam behaves like a third-party ONVIF camera that UniFi Protect 7.3 can adopt and
onvif-recorder can subscribe to. Every behaviour can be changed at runtime through a control
API, so features can be tested against a real Protect without touching a real camera.

The camera provides:

- two H.264 Main-profile RTSP streams, each with an IDR frame every 2 s and RTSP over TCP
  only, with Digest auth:

  | Stream | Resolution | Frame rate | Bitrate cap |
  |---|---|---|---|
  | `stream1` | 1280x720 | 10 fps | 800 kbps |
  | `stream2` | 848x480 | 10 fps | 250 kbps |

- a test pattern (SMPTE bars) with a UTC clock, a frame counter and a status line in the
  picture;
- `GET /snapshot.jpg` with Digest auth: the latest stream1 frame, refreshed twice a second;
- ONVIF services:
  - always on: device, media (ver10) and events (PullPoint);
  - on by default: deviceIO and imaging;
  - off by default: media2, PTZ and analytics.
- events on demand for every topic onvif-recorder handles;
- scenarios that put a person, vehicle, dog or parcel into the picture and emit motion
  events, so NanoDet-M in the recorder has a real subject to classify.

| | |
|---|---|
| Identity | Manufacturer `MockCam`, Model `ONVIF-SIM-1` (Protect names it "MockCam ONVIF-SIM-1"), serial `MOCKCAM0001`, HwAddress `02:4D:4F:43:4B:01` |
| Image | `127.0.0.1:30500/onvif-mockcam:<gitrev>` (and `:dev`), built by `build.sh` |
| Homelab deployment | `/mnt/cephfs/ssd/homelab/k8s/onvif-mockcam.yaml`: namespace `onvif-test`, MetalLB `192.168.3.216` on VLAN 3 |
| Credentials | Kubernetes Secret `onvif-test/mockcam-credentials` (`username`, `password`), copied from `/mnt/cephfs/ssd/homelab/.credentials/onvif-mockcam` (`user=`, `password=`) |

## Endpoints

| URL (as deployed) | Container port | What |
|---|---|---|
| `http://192.168.3.216/onvif/...` | 8000 | ONVIF SOAP 1.2 |
| `http://192.168.3.216/snapshot.jpg` | 8000 | JPEG, Digest auth |
| `rtsp://192.168.3.216/stream1`, `/stream2` | 8554 | RTSP (mediamtx), TCP interleaved only, Digest auth |
| `http://192.168.3.216:8080/` | 8080 | Control API, **no authentication** (VLAN 3 only) |

SOAP paths are `/onvif/device_service`, `/onvif/media_service`, `/onvif/media2_service`,
`/onvif/event_service`, `/onvif/deviceio_service`, `/onvif/ptz_service`,
`/onvif/imaging_service`, `/onvif/analytics_service` and `/onvif/subscription/<id>`.

Requests are dispatched on the first element in the SOAP Body, with its namespace. Protect
sends no SOAPAction header, so the header is only logged. XAddrs, stream and snapshot URIs,
and subscription addresses are absolute URLs built from `MOCKCAM_ADVERTISE_HOST` (or from the
request's `Host` header when that variable is unset).

## Adding it to Protect

The router does not run WS-Discovery on VLAN 3, so the camera has to be added by IP. This is
a write on the console, so only do it deliberately. The credentials are in
`/mnt/cephfs/ssd/homelab/.credentials/onvif-mockcam`. Two ways to add it:

- Protect UI: add a third-party camera with IP `192.168.3.216` and those credentials.
- Through the API, on the router:

  ```bash
  ssh root@192.168.1.1 'curl -sS -X POST http://127.0.0.1:7080/api/third-party-cameras/adopt \
    -H "Content-Type: application/json" -H "X-Source: unifi-os" \
    -H "X-UserId: $(cat /var/lib/onvif-recorder/protect-user-id)" \
    -d "{\"host\":\"192.168.3.216\",\"username\":\"<user>\",\"password\":\"<password>\"}"'
  ```

What to expect afterwards:

- **Protect** logs `Connected to ONVIF camera at 192.168.3.216:80 (media2=false, profiles=2)`
  in `onvif.log` and `Started ONVIF event subscription` in `onvif.events.log`.
- **onvif-recorder** picks the camera up within 60 s (`[rescan]`).
- **Recording:** Protect records it continuously (`mode=always`). That is a few GB a day:
  the static pattern encodes at about 200 kbps per stream, against caps of 800 and
  250 kbps. Set it to detections-only recording if that matters.
- **Removal:** `DELETE /api/cameras/<id>`, or "Forget" in the UI.

Before adopting, check the camera from this machine with Protect's own code (see
[Pre-flight with Protect's code](#pre-flight-with-protects-code)).

## Control API

Base URL `http://192.168.3.216:8080` (`GET /` lists the endpoints).

| Method and path | What |
|---|---|
| `GET /healthz` | 200 when both RTSP paths are live and the snapshot is less than 10 s old (the readiness probe) |
| `GET /mock/state` | Everything in one place: identity, knobs, encoder settings, event states, subscriptions, RTSP readers (Protect's `ms` shows up here), recent ffmpeg log |
| `GET /mock/config` | Current knobs |
| `POST /mock/config` | Change knobs: `{"knob": value, ...}`; unknown knobs and bad values return 400 |
| `POST /mock/config/reset` | All knobs back to their defaults |
| `POST /mock/event` | Emit one event (see [Events](#events)) |
| `POST /mock/scenario` | Show a subject and emit its events (see [Scenarios](#scenarios)) |
| `GET /mock/events` | Event kinds, the current state of every stateful topic, the last 50 messages emitted |
| `GET /mock/requests?since=<seq>&op=<Operation>&limit=<n>` | Log of the last 1000 SOAP and snapshot requests |
| `DELETE /mock/requests` | Clear the request log |
| `GET /mock/subscriptions` | Live pull-point subscriptions, plus the last 20 that ended and why |
| `DELETE /mock/subscriptions` | Drop every subscription, as a camera reboot would |
| `GET /snapshot.jpg` | The snapshot without authentication |

Each request-log entry has these fields:

- `seq`, `t`, `client`, `method`, `path`;
- `op`, `ns`, `service`;
- `auth`: `pre-auth`, `wsse`, `http-digest`, `missing`, or `wsse-failed: <reason>`;
- `user`, `wsa_to`, `wsa_action`, `ref_params`;
- `headers`: the local names of the SOAP header blocks;
- `soap_action`, `user_agent`;
- `status`, `fault`, `ms`;
- `injected`, when a `fault_next` fired.

### Knobs

| Knob | Default | Effect |
|---|---|---|
| `media2` | `false` | Also serve ver20 media at `/onvif/media2_service` (Protect then uses media2) |
| `ptz` | `false` | ver20 PTZ service: `GetStatus`, `ContinuousMove`, `AbsoluteMove`, `RelativeMove`, `Stop`, presets and home. Position shows in the status line |
| `imaging` | `true` | ver20 imaging service: `GetOptions`, `GetImagingSettings`, `SetImagingSettings` |
| `deviceio` | `true` | deviceIO service: digital input `DI_1`, relay `RO_1`. `SetRelayOutputState` emits `tns1:Device/Trigger/Relay` |
| `analytics` | `false` | ver20 analytics service with a `tt:CellMotionEngine` module, and `VAC_1` in the profiles |
| `analytics_sensitivity` | `50` | Sensitivity of that module. `0` makes Protect mark motion detection "misconfigured" |
| `events` | `true` | `false` makes the camera have no event service at all |
| `motion_topics` | `true` | `false` leaves MotionAlarm and CellMotion out of `GetEventProperties`. With `analytics` off, Protect then reports motion "unsupported" |
| `audio` | `false` | Advertise an AAC audio encoder in both profiles (Protect sets `hasMic`). The streams still carry no audio |
| `auth_mode` | `wsse` | See below |
| `created_window_s` | `0` | When greater than 0, reject a UsernameToken whose `Created` is further than this from the camera clock |
| `clock_skew_s` | `0` | Seconds added to the camera clock. Protect's `SetSystemDateAndTime` (Manual) sets it too |
| `slots` | `0` | Maximum live pull-point subscriptions (0 = unlimited). When full, Create returns the captured Dahua `ter:BlockedThreadUpperLimit` fault |
| `wsa_strict` | `false` | PullMessages, Renew and Unsubscribe without `wsa:To` and `wsa:Action` get HTTP 400 `ter:InvalidArgs`, as Reolink does (#58) |
| `ref_params_required` | `false` | New subscriptions share the address `/onvif/subscription` and are identified only by `ReferenceParameters` (`mc:SubscriptionId`), as Axis does. Requests without them get 400 |
| `notauthorized_on_create` | `false` | `CreatePullPointSubscription` always fails with 400 `ter:NotAuthorized`, as some Hikvision firmware does |
| `pull_hold` | `true` | Hold `PullMessages` until a message arrives or the requested `Timeout` passes. `false` answers at once |
| `initial_state` | `true` | Queue `Initialized` messages with the current state of every stateful topic when a subscription is created. `SetSynchronizationPoint` always does |
| `fault_next` | `null` | Inject a fault, delay or connection drop into upcoming requests (see below) |
| `encoder_set` | `apply` | `SetVideoEncoderConfiguration`: `apply` restarts the publisher with the new resolution, frame rate, bitrate or GOP; `fault` refuses with `ter:ConfigModify` |
| `stream_up` | `true` | `false` stops the RTSP publisher, so the paths go offline. Protect loses video; ONVIF keeps working |
| `snapshot_auth` | `digest` | `digest`, `basic`, `any` (challenge Digest, also accept Basic) or `none` |
| `subject` | `none` | `none`, `person`, `vehicle`, `animal` or `package`: a picture shown in the video and snapshot until changed |

`auth_mode` controls SOAP authentication. `GetSystemDateAndTime` without credentials is
always allowed:

- `wsse` (the default):
  - A WS-UsernameToken (PasswordDigest or PasswordText) or HTTP Digest is accepted.
  - A bad token gets 400 `ter:NotAuthorized`.
  - No credentials get 401 with a Digest challenge, which is what Protect's Digest fallback
    probes for.
- `wsse_only`: as `wsse`, but there is never a challenge, so missing credentials also get
  400 `ter:NotAuthorized`.
- `digest`:
  - Only HTTP Digest is accepted; a request without it gets a 401 challenge.
  - Protect falls back to Digest and logs `ONVIF Digest fallback resolved`.
  - onvif-recorder, which has no Digest fallback, cannot subscribe.
- `none`: no authentication.

`fault_next` takes an object with these keys:

- `op`: an operation name, or `*` for any. Default `*`.
- `count`: how many matching requests it applies to. Default 1.
- `status`: the HTTP status to answer with. Default 500; 0 means no fault, only the delay.
- `code`, `subcode`, `reason`: the SOAP fault to return. Defaults `Receiver`, `ter:Action`,
  `Injected fault`.
- `delay_s`: a delay before answering, 0 to 120 s.
- `close`: `true` drops the connection without a response.

The fault is applied after authentication and consumed when it fires. For example:

```bash
# The next 3 PullMessages fail with HTTP 500: Protect counts 3 errors and deactivates.
curl -s -XPOST 192.168.3.216:8080/mock/config \
  -d '{"fault_next":{"op":"PullMessages","count":3,"status":500}}'
# Protect's 10 s request timeout on the next Renew.
curl -s -XPOST 192.168.3.216:8080/mock/config \
  -d '{"fault_next":{"op":"Renew","delay_s":12,"status":0}}'
```

### Events

`POST /mock/event` takes these fields; all are optional except `kind`:

| Field | Meaning |
|---|---|
| `kind` | See the table below |
| `state` | `true` or `false`, for stateful kinds. Default `true` |
| `duration_s` | Revert to `false` after this long. When `state` is omitted, the default is a 5 s pulse |
| `class` | `ClassTypes` for `field` and `line`: `Human`, `Vehicle`, `Animal`, `Face`, `LicensePlate`, or `""` to omit it. Default `Human` |
| `rule` | `Rule` source item for `field`, `line` and `cell_motion` |
| `object_id` | `ObjectId` for `field` and `line` |
| `bbox` | `[left, top, right, bottom]` in ONVIF -1..1 coordinates, sent as `tt:BoundingBox` |
| `token` | The first Source item, e.g. a different `InputToken` |
| `source`, `data` | Extra or replacement SimpleItems: `{"Name": "Value"}` |
| `topic` | Replaces the topic string |
| `property_op` | Replaces `PropertyOperation` (`Changed` by default; `Initialized` is ignored by the recorder for line crossings) |
| `raw_xml` | Instead of everything above: a complete `wsnt:NotificationMessage`, sent verbatim. The prefixes `wsnt`, `tt` and `tns1` are declared on the envelope |

| `kind` | Topic | Source / Data | What the recorder does |
|---|---|---|---|
| `motion_alarm` | `tns1:VideoSource/MotionAlarm` | Source=VideoSource_1 / State | Protect motion; recorder fallback only when the camera has sent no CellMotion or AI topic |
| `cell_motion` | `tns1:RuleEngine/CellMotionDetector/Motion` | VSC/VAC/Rule / IsMotion | Protect motion; recorder generic motion, classified by NanoDet-M from the snapshot |
| `cell_motion_bugtopic` | `tns1:RuleEngine/tt:CellMotionDetector` | Source / IsMotion | As `cell_motion` (a firmware bug's topic spelling) |
| `field` | `tns1:RuleEngine/FieldDetector/ObjectsInside` | VSC/VAC/Rule / ObjectId, IsInside, ClassTypes (+ bbox) | Typed start and stop |
| `line` | `tns1:RuleEngine/LineDetector/Crossed` | VSC/VAC/Rule / ObjectId, ClassTypes (+ bbox) | Momentary event (no state) |
| `people`, `vehicle`, `face`, `dogcat`, `visitor`, `package` | `tns1:RuleEngine/MyRuleDetector/{PeopleDetect,VehicleDetect,FaceDetect,DogCatDetect,Visitor,Package}` | Source / State | person, vehicle, person, animal, person, package |
| `human_shape` | `tns1:UserAlarm/IVA/HumanShapeDetect` | VSC / State | person |
| `vehicle_alarm` | `tns1:VehicleAlarm/IVB/VehicleDetect` | VSC / State | vehicle |
| `digital_input` | `tns1:Device/Trigger/DigitalInput` | InputToken=DI_1 / LogicalState true/false | Protect digital input; ignored by the recorder |
| `relay` | `tns1:Device/Trigger/Relay` | RelayToken=RO_1 / LogicalState active/inactive | Ignored by the recorder |
| `noise` | `tns1:Monitoring/ProcessorUsage` | Token / Value | Silently ignored housekeeping |
| `unknown` | `tns1:RuleEngine/MockCam/Unsupported` | Source / State | The recorder's `[unhandled-topic]` warning, once |

Every AI topic (`field`, `line`, the `MyRuleDetector` family, `human_shape`,
`vehicle_alarm`) marks the camera AI-capable in the recorder. From then on the recorder
suppresses that camera's CellMotion and MotionAlarm until it restarts. Protect itself only
reads the motion and digital-input topics.

### Scenarios

`POST /mock/scenario {"kind": "person"|"vehicle"|"animal"|"package"|"none", "seconds": 10}`
runs one detection:

1. It shows the subject in the video and snapshot. The subject reaches the snapshot about
   2.5 s after the change.
2. After `lead_s` (default 3 s) it sets the scenario's events to true.
3. After `seconds` it sets them back to false.
4. After a further `tail_s` (default 2 s) it removes the subject.

The events default to `["cell_motion", "motion_alarm"]`. That is generic motion, so with
`--detect_override` (as on the router) NanoDet-M classifies the snapshot and the subject
decides the class.

Other options:

- `"ai": true` also emits the matching AI topic:
  - person, vehicle and animal: `field` with `ClassTypes` Human, Vehicle or Animal, plus
    `bbox`;
  - package: `package`, because COCO has no parcel class and NanoDet-M cannot see one.

  This marks the camera AI-capable in the recorder (see above).
- `"ai_kind"` chooses a different AI topic.
- `"events"` replaces the list.

A new scenario cancels the one running; `"kind": "none"` clears the subject.

The subjects are the public-domain `test/testdata/security_cam_person.jpg` and
`security_cam_vehicle.jpg`, a CC0 photo of a dog, and a drawn parcel. `subjects/SOURCES.md`
has the details.

## Test recipes

```bash
C=http://192.168.3.216:8080
```

- **Protect motion:**
  `curl -XPOST $C/mock/event -d '{"kind":"cell_motion","duration_s":10}'`. Protect shows a
  10 s motion event. The recorder records a fallback event, classified from the snapshot.
- **Smart detection through NanoDet-M:**
  `curl -XPOST $C/mock/scenario -d '{"kind":"person","seconds":8}'`. The recorder logs a
  NanoDet class `person`, and the event's thumbnail shows the person.
- **Camera-classified detections:**
  `curl -XPOST $C/mock/event -d '{"kind":"field","class":"Vehicle","bbox":[-0.2,-0.2,0.6,0.7],"duration_s":6}'`.
- **Digital input and alarms:**
  `curl -XPOST $C/mock/event -d '{"kind":"digital_input","duration_s":5}'`. Protect publishes
  a digital-input change, the trigger for alarm automations.
- **Reolink (#58):** `curl -XPOST $C/mock/config -d '{"wsa_strict":true}'`.
  - An unpatched Protect fails every pull and ends with motion detection "misconfigured".
  - With `--patch_onvif_addressing` (the default) Protect keeps pulling.
  - `GET $C/mock/requests?op=PullMessages` shows `wsa_to` and `wsa_action` per request.
- **One-slot camera (event sharing):** `{"slots":1}`. Protect and the recorder now compete
  for the slot; add the camera's MAC to `--protect_event_proxy_cameras` to share it.
- **Axis-style ReferenceParameters:** `{"ref_params_required":true}`, then
  `DELETE $C/mock/subscriptions` so both clients resubscribe.
- **Hikvision NotAuthorized on Create:** `{"notauthorized_on_create":true}`.
- **Digest-only camera:** `{"auth_mode":"digest"}`. Protect keeps working through its Digest
  fallback; the recorder cannot subscribe.
- **Camera reboot:** `DELETE $C/mock/subscriptions` drops every subscription; each client
  must create a new one.
- **Stream loss:** `{"stream_up":false}`. Protect loses video while ONVIF still answers.
  - Keep it under a minute: the recorder's `--auto_heal_protect` restarts Protect when a
    managed camera is not recording on two checks a minute apart.
- **Clock skew:** `{"clock_skew_s":3600}`.
  - Protect re-learns the offset on its next connection, and its 6-hourly clock check sets
    the clock back with `SetSystemDateAndTime`.
  - Combine with `{"created_window_s":300}` to see the recorder's tokens rejected: it signs
    with its own clock.
- **Stream settings UI:** changing resolution, frame rate or bitrate in Protect calls
  `SetVideoEncoderConfiguration`. That restarts the publisher with the new settings, and
  `GET $C/mock/state` shows them.

## Pre-flight with Protect's code

`preflight/protect-preflight.cjs` loads Protect's own modules out of a `service.js` and runs
them against the camera. It uses the loader from `scripts/protect-e2e/lib/loader.cjs`. It
covers:

- the adoption probe: `getCameraDetails`, `getStreamData` and `getMAC`;
- the imaging, encoder-options, deviceIO and analytics probes;
- the snapshot fetch through `digest-fetch`;
- the event client: GetEventProperties, Create, Pull, Renew and Unsubscribe, with motion and
  digital-input events triggered through the control API.

With `--scenarios=digest,media2,wsa,refparams,slots` it also checks those knobs.

It needs these npm packages:

- at Protect 7.3.70's versions: `undici@7.24.6`, `fast-xml-parser@5.3.8`,
  `node-cache@5.1.2`, `p-queue@6.6.2`;
- for Protect's response validators and the snapshot: `ajv@8`, `zod@3`,
  `zod-to-json-schema@3`, `typebox@1`, `digest-fetch@2` and `node-fetch@2`.

```bash
mkdir -p ~/protect-npm && (cd ~/protect-npm && npm init -y >/dev/null && npm install \
  undici@7.24.6 fast-xml-parser@5.3.8 node-cache@5.1.2 p-queue@6.6.2 \
  ajv@8 zod@3 zod-to-json-schema@3 typebox@1 digest-fetch@2 node-fetch@2)
node tools/mock-camera/preflight/protect-preflight.cjs --bundle=<service.js> \
  --nm=$HOME/protect-npm/node_modules --host=192.168.3.216 \
  --cred=/mnt/cephfs/ssd/homelab/.credentials/onvif-mockcam \
  --control=http://192.168.3.216:8080 --scenarios=digest,media2,wsa,refparams,slots
```

The exit status is 0 when every check passes. A `service.js` comes from a console
(`/usr/share/unifi-protect/app/service.js`, or the `.bak` beside it for the unpatched
original). Never commit one.

## Development

```bash
cd tools/mock-camera
python3 -m pytest -q tests          # SOAP, auth, events and control API, without media
./build.sh --no-push                # image only
docker run --rm -p 18000:8000 -p 18080:8080 -p 18554:8554 \
  -e MOCKCAM_USERNAME=u -e MOCKCAM_PASSWORD=p -e MOCKCAM_ADVERTISE_HOST=127.0.0.1 \
  -e MOCKCAM_ADVERTISE_HTTP_PORT=18000 -e MOCKCAM_ADVERTISE_RTSP_PORT=18554 \
  127.0.0.1:30500/onvif-mockcam:dev
./build.sh                          # build and push :dev and :<gitrev>
```

Environment variables (all optional except the credentials):

| Variable | Default | Meaning |
|---|---|---|
| `MOCKCAM_USERNAME`, `MOCKCAM_PASSWORD` | (required) | ONVIF, RTSP and snapshot credentials. Use letters and digits only: Protect puts them into the RTSP URL |
| `MOCKCAM_ADVERTISE_HOST` | request `Host` | Host in every URL the camera hands out |
| `MOCKCAM_ADVERTISE_HTTP_PORT`, `MOCKCAM_ADVERTISE_RTSP_PORT` | `80`, `554` | Ports in those URLs |
| `MOCKCAM_HTTP_PORT`, `MOCKCAM_CONTROL_PORT`, `MOCKCAM_RTSP_PORT` | `8000`, `8080`, `8554` | Listening ports in the container |
| `MOCKCAM_KNOBS` | | JSON object of knob values to start with |
| `MOCKCAM_PATTERN` | `smptehdbars` | `smptehdbars`, `testsrc2` (moving) or `testsrc` |
| `MOCKCAM_MAC`, `MOCKCAM_SERIAL`, `MOCKCAM_MANUFACTURER`, `MOCKCAM_MODEL` | as above | Identity. Avoid "hikvision", "dahua" and "amcrest": Protect and the recorder special-case them |
| `MOCKCAM_MEDIA` | `1` | `0` runs only the SOAP and control servers (used by the tests) |

How it fits together:

- The image is `bluenviron/mediamtx:1.21.2-ffmpeg`, pinned by digest, which gives
  mediamtx 1.21.2 and ffmpeg 8.1.2 on Alpine 3.24. Python 3.14, the DejaVu font and tini are
  added on top.
- The Python process (`mockcam/`, standard library only) serves SOAP, the snapshot and the
  control API. It also supervises mediamtx and ffmpeg, writing mediamtx's config from the
  credentials.
- ffmpeg composes the pattern with the subject overlay, a PNG that it re-reads every frame.
  It draws the text and encodes both streams, publishing them to mediamtx on localhost. It
  also writes the snapshot JPEG.

Limitations:

- No WS-Discovery: the camera is added by IP.
- No audio track.
- RTSP over UDP is refused.
- No push (`Subscribe`) notifications, only PullPoint.
- One video source.
