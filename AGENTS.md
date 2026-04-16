# AGENTS.md — Codebase guide for AI agents

This file describes the purpose, structure, and library usage of the ONVIF
event recorder project so that AI coding agents can navigate it effectively.

---

## What this project does

The recorder bridges third-party ONVIF IP cameras into a UniFi Protect
installation.  At runtime it:

1. Reads camera credentials from the UniFi Protect PostgreSQL database
   (`cameras` table, `isThirdPartyCamera = true`).
2. Opens a WS-PullPoint subscription to each camera over HTTP/SOAP.
3. Translates raw ONVIF detection events (human / vehicle) into SQLite rows
   that mirror the UniFi Protect `events` and `smartDetectObjects` schema.
4. Optionally fetches a JPEG snapshot from each camera on detection start and
   appends it to a per-camera `.ubv` thumbnail file.
5. Writes every raw SOAP exchange to a timestamped `.jsonl` file for
   replay-based testing.

---

## Source files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Entry point. Loads cameras from UniFi Protect DB, wires up `OnvifListener` + `DetectionRecorder` + `MotionPoller`, handles SIGINT/SIGTERM. Auto-downloads NanoDet-M models. |
| `src/onvif_listener.hpp/.cpp` | `onvif::OnvifListener` — manages WS-PullPoint subscriptions; one thread per camera. Parses SOAP XML into `OnvifEvent` structs and delivers them via a callback. |
| `src/detection_recorder.hpp/.cpp` | `onvif::DetectionRecorder` — filters events to human/vehicle/animal/package detections, maintains open-event state, writes to PostgreSQL, fetches snapshots via libcurl, crops thumbnails via NanoDet-M or ONVIF bbox. |
| `src/alarm_notifier.hpp/.cpp` | `onvif::AlarmNotifier` — triggers Protect automations on smart detection events via the local Protect API (port 7080). Records automation history with cooldown support. |
| `src/motion_poller.hpp/.cpp` | `onvif::MotionPoller` — polls the events table for first-party camera `motion` events, runs NanoDet-M on Protect thumbnails, and inserts smart detection records. |
| `src/camera_change_log.hpp/.cpp` | `unifi::CameraChangeLog` — thread-safe JSON Lines log of cameras-table mutations (old/new values). Used for rollback support. |
| `src/protect_ui_patch.hpp/.cpp` | `protect_ui::patch_alarm_picker()` / `revert_alarm_picker()` — live-patches the Protect UI (swai*.js, vantage*.js, service.js) so third-party cameras appear in alarm creation. Uses dpkg md5sums for backup integrity. |
| `src/ubv_thumbnail.hpp/.cpp` | `ubv::encode` / `ubv::decode` / `ubv::append` — minimal UBV container for storing JPEG thumbnail frames. |
| `src/jpeg_crop.hpp/.cpp` | JPEG decode/crop/re-encode via libjpeg-turbo. Used for thumbnail cropping from ONVIF bounding boxes and NanoDet-M detections. |
| `src/object_detect.hpp/.cpp` | `onvif::ObjectDetector` — NanoDet-M on-device object detection via NCNN. Returns bounding boxes and COCO class labels. Built with NEON SIMD on ARM64. |
| `src/event_recorder.hpp/.cpp` | `onvif::EventRecorder` — thread-safe JSON Lines writer for parsed ONVIF events. Used by `--event_log`. |
| `src/util.hpp/.cpp` | `onvif::util::` — shared helpers: UUID v4 generation, 24-char hex ID generation, ISO-8601 timestamps, JSON string escaping. |
| `src/unifi_camera_config.hpp/.cpp` | `unifi::load_cameras()` — queries the UniFi Protect PostgreSQL instance and returns a `CameraConfig` per adopted third-party camera. Also handles smart-detect flag enablement, rollback, and first-party camera discovery. |
| `src/ubv_extract.cpp` | Standalone tool to extract JPEG frames from UBV thumbnail files. |
| `test/onvif_camera_emulator.hpp/.cpp` | HTTP server (libmicrohttpd) that replays raw `.jsonl` SOAP logs; used as a fake camera in tests. |
| `test/camera_emulators.hpp/.cpp` | Concrete emulators for Camera 108 (FieldDetector) and Camera 109 (UserAlarm/IVA). |
| `test/test_onvif_listener.cpp` | Drives `OnvifListener` against emulated cameras; JSONL path passed automatically by Bazel. |
| `test/test_detection_recorder.cpp` | End-to-end test: emulated camera → `DetectionRecorder` → PostgreSQL-like assertions. |
| `test/test_ubv_thumbnail.cpp` | Round-trip test: encodes snapshot JPEGs into a UBV file, decodes, verifies fidelity. |
| `test/test_camera_change_log.cpp` | CameraChangeLog write/read roundtrip, concurrent writes, malformed line recovery. |
| `test/test_protect_ui_patch.cpp` | Protect UI patch apply/revert logic, dpkg md5sum backup validation. |
| `test/test_unifi_camera_config.cpp` | DB connection string / JSON extraction / PG array literal helper tests. |
| `test/test_motion_poller.cpp` | Smart detect type mapping / SDR payload JSON generation tests. |
| `test/testdata/` | Test fixtures: snapshot JPEGs, JSONL replay logs for multiple camera brands. |

---

## Key types

### `onvif::OnvifEvent`
Delivered to the `EventCallback` for every received event:
```cpp
struct OnvifEvent {
    std::string camera_ip;
    std::string camera_user;
    std::string topic;        // e.g. "tns1:RuleEngine/FieldDetector/ObjectsInside"
    std::string event_time;   // camera-reported UTC timestamp
    std::string property_op;  // "Initialized", "Changed", or "Deleted"
    std::map<std::string, std::string> source;
    std::map<std::string, std::string> data;
};
```

### `onvif::CameraConfig`
```cpp
struct CameraConfig {
    std::string id;           // UUID from the cameras table (empty for SQLite-only use)
    std::string mac;          // MAC address, uppercase no colons e.g. "FC5F49CA68D4"
    std::string ip;
    std::string user;
    std::string password;
    std::string snapshot_url;      // optional HTTP URL for JPEG snapshot fetch
    int retry_interval_sec{10};
    int max_consecutive_failures{0};  // 0 = unlimited retries
};
```

---

## Detection event formats (two camera styles)

**Camera 108** — `tns1:RuleEngine/FieldDetector/ObjectsInside`
- `source["Rule"]` = `"Human"` | `"Vehicle"`
- `data["IsInside"]` = `"true"` | `"false"`

**Camera 109** — `tns1:UserAlarm/IVA/HumanShapeDetect`
- `data["State"]` = `"true"` | `"false"` (always maps to person)

ONVIF `"Human"` → SQLite `"person"`;  ONVIF `"Vehicle"` → SQLite `"vehicle"`.

---

## External libraries

| Library | Used by | Purpose |
|---------|---------|---------|
| **libcurl** | `src/onvif_listener.cpp`, `src/detection_recorder.cpp`, `src/alarm_notifier.cpp`, `src/main.cpp` | HTTP/SOAP POST to cameras; JPEG snapshot fetch; Protect API calls; model download. |
| **libxml2** | `src/onvif_listener.cpp` | Parse SOAP XML responses from cameras; extract topic, source/data key-value pairs. |
| **openssl** | Transitive (libcurl) | TLS for HTTPS camera endpoints. |
| **libpq** | `src/unifi_camera_config.cpp`, `src/detection_recorder.cpp`, `src/alarm_notifier.cpp`, `src/motion_poller.cpp` | Query the UniFi Protect PostgreSQL database: camera config, event writes, automation history, thumbnail fetch. |
| **libjpeg-turbo** | `src/jpeg_crop.cpp`, `src/detection_recorder.cpp`, `src/motion_poller.cpp` | JPEG decode/crop/re-encode for thumbnail processing. |
| **NCNN** | `src/object_detect.cpp` | NanoDet-M neural network inference for on-device object detection. Built from source for x86 and ARM64 (NEON SIMD). |
| **libmicrohttpd** | `test/` only | Embedded HTTP server used by the camera emulator in tests to serve canned SOAP responses. |

### Library roles in detail

**libcurl** (`OnvifListener`)
- Sends `CreatePullPointSubscription` and repeated `PullMessages` SOAP
  requests to each camera.
- Handles HTTP Digest authentication automatically.
- Each camera runs in its own thread with its own `CURL*` handle.

**libxml2** (`OnvifListener`)
- Parses the SOAP envelope returned by `PullMessages`.
- Walks `NotificationMessage` elements to extract `Topic`, `Source`, and
  `Data` key-value pairs.
- All XML documents are freed after each parse; no persistent DOM state.

**libpq** (`src/unifi_camera_config.cpp`)
- Single synchronous connection to the Protect PostgreSQL database
  (default: Unix socket `/run/postgresql`, port 5433).
- Queries `cameras` table; extracts `host` (IP) and credentials from the
  `thirdPartyCameraInfo` JSONB column using `PQgetvalue`.
- Also enables smart-detect flags and handles rollback.

**libpq** (`DetectionRecorder`, `MotionPoller`, `AlarmNotifier`)
- Writes smart detection events, thumbnails, and automation history into
  the live UniFi Protect database.
- Thumbnails inserted directly into the `thumbnails` table as `bytea`.
- `thumbnailId` is a 24-char hex string — UniFi Protect's UI routes IDs of
  exactly 24 chars to the DB; any other length goes to msp TCP and fails.

**libcurl** (`DetectionRecorder`)
- On detection start, fetches a JPEG snapshot from `CameraConfig::snapshot_url`
  using HTTP Digest auth.

**libcurl** (`AlarmNotifier`)
- Sends `POST /api/automations/{id}/run` to the local Protect API (port 7080)
  to trigger configured automations on smart detection events.
- Uses `X-UserId` + `X-Source: unifi-os` headers for localhost auth bypass.

**NCNN** (`ObjectDetector`)
- NanoDet-M model loaded from `--model_dir` (auto-downloaded on first start).
- Single-threaded inference (~158 ms on Dream Router ARM64 cores).
- Returns bounding boxes and COCO class labels used for thumbnail cropping
  and object type classification.

**libmicrohttpd** (tests only)
- `OnvifCameraEmulator` listens on a loopback port and serves pre-recorded
  SOAP responses read from a `.jsonl` file, allowing deterministic replay
  without real hardware.

---

## PostgreSQL schema (key tables)

The recorder writes directly into the UniFi Protect PostgreSQL database.
Key tables used:

- **`events`** — `id` (UUID v4), `type` ('smartDetectZone'), `start`/`"end"` (ms epoch),
  `"cameraId"`, `"smartDetectTypes"` (JSON array), `"thumbnailId"` (24-char hex),
  `metadata` (JSONB, `{"source":"onvif-recorder"}` marks our events).
- **`smartDetectObjects`** — per-detection-type records linked to events.
- **`smartDetectRaws`** — raw detection payloads (zones, scores).
- **`thumbnails`** — `id` (24-char hex), `content` (bytea JPEG).
- **`detectionLabels`** — label records for detection filtering.
- **`automationsHistory`** — alarm trigger history with cooldown tracking.

Note: `"end"` must be double-quoted in SQL because `END` is a reserved word.

---

## Git policy

**Never use `git push --no-verify`.** The pre-push hook runs lint, tests, and the PGO
benchmark to protect the repository from broken commits. If the hook fails, fix the
underlying issue. If GitHub drops the SSH connection due to timeout while the hook
runs, investigate and resolve the cause — bypassing the hook is not an option.

## Release checklist

Every GitHub release **must** include these assets:
- `onvif_recorder_arm64` — ARM64 release binary (PGO + ThinLTO) built at the tagged commit
- `onvif-recorder.service` — systemd service file

Steps:
1. Tag the commit: `git tag v<X.Y.Z> && git push origin v<X.Y.Z>`
2. Build: `scripts/bz build --config=arm64_release //:onvif_recorder`
3. Create release with `gh release create` and upload the binary from
   `~/.cache/bazel/arm64_release/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder`
   plus `onvif-recorder.service`.

Do not create a release without attaching the ARM64 binary and service file.

---

## Build system

Bazel is used exclusively.  See `README.md` for full build instructions.

- `bazel/pkg_config.bzl` — repository rule that calls `pkg-config` to locate
  each system library and resolves `-lXXX` flags to full `.a` paths for
  static linking.
- `bazel/arm64_sysroot.bzl` — repository rule that downloads ~36 Ubuntu `.deb`
  packages and synthesises an aarch64-linux-gnu sysroot + clang cross-toolchain.
- `stubs/libsasl2.a` — minimal Cyrus SASL stub (x86_64) so libldap links
  statically without requiring `libsasl2-dev`.
- `.bazelrc` — sets `-std=c++17 -Wall -Wextra -O2 -pthread`; defines
  `--config=arm64` alias for cross-compilation.

---

## Coding conventions

- C++17 throughout.
- Google C++ Style Guide enforced via cpplint (`CPPLINT.cfg` at project root,
  line length 100). Run `python3 -m cpplint <file>` to check.
- All public APIs are in the `onvif::` or `unifi::` namespaces.
- No exceptions cross thread boundaries; each camera thread catches internally.
- Callbacks must be thread-safe (called from camera threads).
- Raw SOAP XML and HTTP responses are never retained beyond a single
  `PullMessages` round trip.
