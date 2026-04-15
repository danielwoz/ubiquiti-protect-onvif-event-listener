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
| `main.cpp` | Entry point. Loads cameras from UniFi Protect DB, wires up `OnvifListener` + `DetectionRecorder`, handles SIGINT/SIGTERM. |
| `onvif_listener.hpp/.cpp` | `onvif::OnvifListener` — manages WS-PullPoint subscriptions; one thread per camera. Parses SOAP XML into `OnvifEvent` structs and delivers them via a callback. |
| `detection_recorder.hpp/.cpp` | `onvif::DetectionRecorder` — filters events to human/vehicle detections, maintains open-event state, writes to SQLite or PostgreSQL, fetches snapshots via libcurl. |
| `ubv_thumbnail.hpp/.cpp` | `ubv::encode` / `ubv::decode` / `ubv::append` — minimal UBV container for storing JPEG thumbnail frames (SQLite backend). |
| `unifi_camera_config.hpp/.cpp` | `unifi::load_cameras()` — queries the UniFi Protect PostgreSQL instance and returns a `CameraConfig` per adopted third-party camera. |
| `test/onvif_camera_emulator.hpp/.cpp` | HTTP server (libmicrohttpd) that replays raw `.jsonl` SOAP logs; used as a fake camera in tests. |
| `test/camera_emulators.hpp/.cpp` | Concrete emulators for Camera 108 (FieldDetector) and Camera 109 (UserAlarm/IVA). |
| `test/test_onvif_listener.cpp` | Drives `OnvifListener` against emulated cameras; JSONL path passed automatically by Bazel. |
| `test/test_detection_recorder.cpp` | End-to-end test: emulated camera → `DetectionRecorder` → SQLite assertions. |
| `test/test_ubv_thumbnail.cpp` | Round-trip test: encodes snapshot JPEGs into a UBV file, decodes, verifies fidelity. |
| `test/testdata/` | Test fixtures: `snapshot_108.jpg`, `snapshot_109.jpg`, `onvif_raw_20260219_085752.jsonl`. |

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
| **libcurl** | `onvif_listener.cpp`, `detection_recorder.cpp` | HTTP/SOAP POST to camera endpoints (Subscribe, PullMessages); JPEG snapshot fetch with HTTP Digest auth. |
| **libxml2** | `onvif_listener.cpp` | Parse SOAP XML responses from cameras; extract topic, source/data key-value pairs. |
| **openssl** | Transitive (libcurl) | TLS for HTTPS camera endpoints. |
| **libpq** | `unifi_camera_config.cpp` | Query the UniFi Protect PostgreSQL database to load adopted camera credentials. |
| **sqlite3** | `detection_recorder.cpp` | Store detection events in a local database mirroring the UniFi Protect schema. |
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

**libpq** (`unifi_camera_config.cpp`)
- Single synchronous connection to `postgres://127.0.0.1:5433/unifi-protect` by default (override with `ONVIF_DB_HOST`).
- Queries `cameras` table; extracts `host` (IP) and credentials from the
  `thirdPartyCameraInfo` JSONB column using `PQgetvalue`.

**sqlite3** (`DetectionRecorder`)
- Schema auto-created on construction if not present.
- Two tables: `events` and `smartDetectObjects`.
- All writes are serialised behind a `std::mutex`; the listener may call
  `on_event()` from multiple camera threads simultaneously.
- `thumbnailId` format: `<camera_ip>-<start_ms>`; JPEG appended to
  `<ubv_dir>/<camera_ip>_thumbnails.ubv`.

**libpq** (`DetectionRecorder` PostgreSQL backend)
- Writes events into the live UniFi Protect database.
- Thumbnails inserted directly into the `thumbnails` table as `bytea`.
- `thumbnailId` is a 24-char hex string — UniFi Protect's UI routes IDs of
  exactly 24 chars to the DB; any other length goes to msp TCP and fails.

**libcurl** (`DetectionRecorder`)
- On detection start, fetches a JPEG snapshot from `CameraConfig::snapshot_url`
  using HTTP Digest auth.

**libmicrohttpd** (tests only)
- `OnvifCameraEmulator` listens on a loopback port and serves pre-recorded
  SOAP responses read from a `.jsonl` file, allowing deterministic replay
  without real hardware.

---

## SQLite schema (abridged)

```sql
CREATE TABLE events (
    id TEXT PRIMARY KEY,           -- UUID v4
    type TEXT,                     -- 'smartDetectZone'
    start INTEGER,                 -- ms epoch
    "end" INTEGER,                 -- ms epoch; NULL while active
    cameraId TEXT,                 -- camera IP
    smartDetectTypes TEXT,         -- JSON array e.g. '["person"]'
    thumbnailId TEXT,              -- '<cameraIP>-<start_ms>'
    createdAt TEXT NOT NULL,
    updatedAt TEXT NOT NULL
    -- ... additional UniFi Protect compatibility columns
);

CREATE TABLE smartDetectObjects (
    id TEXT PRIMARY KEY,           -- UUID v4
    eventId TEXT NOT NULL,         -- → events.id
    cameraId TEXT NOT NULL,
    type TEXT NOT NULL,            -- 'person' | 'vehicle'
    detectedAt INTEGER NOT NULL,   -- ms epoch
    -- ...
);
```

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
