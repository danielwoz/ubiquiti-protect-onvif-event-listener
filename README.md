# ONVIF Event Recorder

Bridges **third-party ONVIF cameras** into UniFi Protect. It listens to ONVIF
WS-PullPoint event streams and writes detection events (person, vehicle, animal,
package) to the UniFi Protect PostgreSQL database so they appear natively in the
Protect UI — including timeline clips, thumbnails, smart detection search, and
security alarms.

> **Note:** This tool is designed for third-party ONVIF cameras but can also
> add smart detection to **first-party UniFi cameras that lack native AI**
> (e.g. G3 Instant). See [First-party camera support](#first-party-camera-support).

---

## Installation on Dream Router / Dream Machine (third-party cameras only)

### Step 1 — Enable SSH on your UniFi device

Go to **UniFi OS → System → Advanced** and enable SSH.
Full instructions: https://help.ui.com/hc/en-us/articles/204909374

### Step 2 — Download the latest release

From your local machine, download the two files from the
[latest release](https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/releases/latest):

```bash
# Copy binary and service file to the router
scp onvif_recorder_arm64 root@<router-ip>:/root/onvif_recorder
scp onvif-recorder.service root@<router-ip>:/etc/systemd/system/
```

### Step 2b — (Optional) Enable NanoDet-M object detection

By default the recorder writes every detection as type **person**.  If your
cameras use generic motion events (CellMotionDetector / VideoSource/MotionAlarm)
rather than AI-specific events, you can enable NanoDet-M so the recorder
automatically classifies each snapshot as **person**, **vehicle**, or **animal**.

> **Performance guidance (Dream Router — 4 × Cortex-A55 cores):**
> NanoDet-M takes ~158 ms per inference on one core (ARM64 NEON build).
> At **1 detection/minute** this is 0.26% of 1 core ≈ **0.065% of total CPU**.
> Even at 5 detections/minute across multiple cameras you are well under 0.5%
> of total device CPU — safe to run alongside all normal Protect workloads.

**Download the model files** (once, from your local machine):

```bash
# Download from ncnn-assets
curl -LO https://github.com/nihui/ncnn-assets/raw/master/models/nanodet_m.param
curl -LO https://github.com/nihui/ncnn-assets/raw/master/models/nanodet_m.bin

# Copy to router
ssh root@<router-ip> "mkdir -p /root/models"
scp nanodet_m.param nanodet_m.bin root@<router-ip>:/root/models/
```

**Install the NanoDet-M service file instead of the standard one:**

```bash
scp onvif-recorder.ncnn.service root@<router-ip>:/etc/systemd/system/
```

Then in [Step 3](#step-3--enable-and-start-the-service), substitute
`onvif-recorder.ncnn` for `onvif-recorder`:

```bash
systemctl enable onvif-recorder.ncnn
systemctl start onvif-recorder.ncnn
systemctl status onvif-recorder.ncnn
```

> The `.ncnn.service` file pre-configures `--detect_override --model_dir=/root/models`.
> It is a drop-in replacement — do **not** enable both service files at the same time.

### Step 3 — Enable and start the service

```bash
chmod +x /root/onvif_recorder
systemctl enable onvif-recorder
systemctl start onvif-recorder
systemctl status onvif-recorder
```

Detections will appear in UniFi Protect within seconds of the first motion event.
Service logs are written to `/var/log/onvif-recorder.log` (errors only by default).

---

## Configuration flags

All options are set via command-line flags. The service file passes no flags by
default, so all behaviour described below is the out-of-the-box default.

To change a flag for the service, edit `/etc/systemd/system/onvif-recorder.service`
and append flags to the `ExecStart` line, then reload:

```bash
# Example: enable verbose logging and write a raw diagnostic log
ExecStart=/root/onvif_recorder --verbose --raw_log=/var/log/onvif-raw.jsonl
```

```bash
systemctl daemon-reload && systemctl restart onvif-recorder
```

### Detection object types

The recorder maps ONVIF camera events to four UniFi Protect smart-detection types:

| Type | How it is produced |
|------|--------------------|
| `person` | FieldDetector "Human", HumanShapeDetect, Reolink PeopleDetect, or any generic motion event when `--default_object_type=person` (the factory default) |
| `vehicle` | FieldDetector "Vehicle", VehicleDetect, Reolink VehicleDetect, or generic motion when `--default_object_type=vehicle` |
| `animal` | Generic motion when `--default_object_type=animal`, or any camera whose events are overridden via `--camera_object_types` |
| `package` | Generic motion when `--default_object_type=package`, or any camera whose events are overridden via `--camera_object_types` |

**Generic motion events** (CellMotionDetector, VideoSource/MotionAlarm) carry no object class. By default the recorder uses `--default_object_type` for these. When NanoDet-M is enabled (`--detect` or `--detect_override`), the detected COCO class overrides the default type — so a snapshot containing a car is stored as `vehicle` even though the camera reported only "motion". Per-camera `--camera_object_types` overrides always take priority over NanoDet-M.

AI-specific events (Human/Vehicle detections from FieldDetector, HumanShapeDetect, etc.) are never affected by `--default_object_type` or NanoDet-M class inference.

**Per-camera overrides** (`--camera_object_types`) replace the detection type for every event from the named camera, including AI events. This is useful for wildlife cameras or entrance monitors where you always want a specific type regardless of what the camera reports.

| Flag | Default | Description |
|------|---------|-------------|
| `--default_object_type` | `person` | Object type written for generic motion events (CellMotionDetector, VideoSource/MotionAlarm) that carry no object class. Valid values: `person`, `vehicle`, `animal`, `package`. |
| `--camera_object_types` | _(disabled)_ | Comma-separated `ip=type` overrides applied to all events from each named camera, e.g. `192.168.1.108=animal,192.168.1.109=package`. Takes precedence over both the ONVIF-reported type and `--default_object_type`. |

**Example — wildlife camera on `.108` and package camera on `.109`:**
```bash
ExecStart=/root/onvif_recorder \
  --default_object_type=person \
  --camera_object_types=192.168.1.108=animal,192.168.1.109=package
```

---

### Security alarms

The recorder integrates with the UniFi Protect alarm system so that ONVIF
camera detections can trigger security alarms you configure in
**Protect → Alarms → New alarm**.

**How it works:**

1. At startup the recorder ensures every third-party camera has a non-empty
   `smartDetectZones` list so it appears in the alarm scope picker.
2. The recorder refreshes the list of configured alarms from the UOS automation
   manager every five minutes.
3. When a detection event is recorded, the recorder posts it to the UOS
   automation manager for every alarm whose trigger matches the detected type
   (`person`, `vehicle`, `animal`, or `package`).

**Setup:**

1. In Protect, create a new alarm under **Protect → Alarms → New alarm**.
2. Set the scope to **All smart cameras with detection zones** and choose the
   trigger types you want (person, vehicle, animal, and/or package).
3. Your ONVIF cameras will now appear in the scope list and fire the alarm on
   matching detections.

| Flag | Default | Description |
|------|---------|-------------|
| `--uos_url` | `http://localhost:11010` | Base URL for the UOS external automation manager. Only change this if you are running the recorder off-device (e.g. during development). |

---

### Database

| Flag | Default | Description |
|------|---------|-------------|
| `--db_conn` | `host=/run/postgresql port=5433 dbname=unifi-protect user=postgres` | libpq connection string for the UniFi Protect database. You only need to change this if your database is on a different host or port. |
| `--db_host` | _(empty = Unix socket)_ | Override only the PostgreSQL host used when loading camera credentials from the database. Useful when running the recorder on a different machine than the Dream Router. |

### RTSP audio

UniFi Protect stores an `enableRtspAudio` flag per third-party camera in its database.
The recorder can set this flag at startup for all cameras that report audio capability
(`hasAudio = true`).

| Flag | Default | Description |
|------|---------|-------------|
| `--rtsp_audio` | _(disabled)_ | Set RTSP audio in the Protect database. `enable` turns on audio for all audio-capable cameras; `disable` turns it off. Empty (default) leaves the database unchanged. |

**Example — enable RTSP audio at every startup:**
```bash
ExecStart=/root/onvif_recorder --rtsp_audio=enable
```

Note: this updates the database on every startup. If you only need a one-time change,
set the flag once, restart the service, then remove the flag and restart again.

### First-party camera support

The recorder can add smart detection to **first-party UniFi cameras that lack
native AI** (e.g. UVC G3 Instant). These cameras already generate `motion` events
in Protect with thumbnails — the recorder polls for those events, runs NanoDet-M
on the existing thumbnail, and inserts smart detection records when a person,
vehicle, or animal is found.

**How it works:**

1. On startup the recorder auto-discovers all adopted first-party cameras where
   `featureFlags.hasSmartDetect` is null or false.
2. A background thread polls the `events` table for completed `motion` events
   from those cameras.
3. For each motion event, the recorder fetches the thumbnail Protect already
   stored, runs NanoDet-M, and — if a relevant subject is detected — inserts
   a `smartDetectZone` event with all associated records (thumbnail, smart
   detect object, smart detect raw, alarm notification).

**Requirements:** NanoDet-M must be enabled (`--detect` or `--detect_override`
with `--model_dir`). Without the model, first-party cameras are discovered but
the motion poller does not start.

**Optional: enable the Protect UI smart detection panel** for specific cameras
by passing their database IDs via `--first_party_cameras`. This updates
`featureFlags.smartDetectTypes` and `smartDetectZones` in the cameras table so
the Protect UI shows smart detections for those cameras. Without this flag,
detections are still recorded and alarms still fire — you just won't see them
in the smart detection timeline filter.

To find your camera IDs, run on the router:

```sql
psql -h /run/postgresql -p 5433 -U postgres unifi-protect -c "
SELECT id, name, model
FROM cameras
WHERE \"isThirdPartyCamera\" = false AND \"isAdopted\" = true
ORDER BY name;
"
```

| Flag | Default | Description |
|------|---------|-------------|
| `--first_party_cameras` | _(disabled)_ | Comma-separated camera IDs to enable smart detection flags for in the cameras table. Only needed if you want these cameras to appear in the Protect smart detection UI. |
| `--poll_interval_sec` | `10` | Seconds between motion-event poll cycles for first-party cameras. |

**Example — enable G3 Instant smart detection with UI support:**
```bash
ExecStart=/root/onvif_recorder \
  --detect_override --model_dir=/root/models \
  --first_party_cameras=6713fe0a01583d03e400051c,6713fa9e023c3d03e4000451
```

---

### Change log and rollback

The recorder can log every cameras-table modification it makes to a JSON Lines
file and undo those changes on demand. This is useful for safely experimenting
with camera flag changes and reverting if something goes wrong.

| Flag | Default | Description |
|------|---------|-------------|
| `--change_log` | _(disabled)_ | Path for the cameras-table change log (JSON Lines). Each line records the camera ID, column name, old value, and new value. |
| `--rollback` | _(disabled)_ | Undo cameras-table changes and **exit**. Values: `third_party` (only third-party cameras), `first_party` (only cameras from `--first_party_cameras`), `all` (both). |

When `--rollback` is set, the recorder reads the change log (if it exists),
applies the original values back to the database, and exits. If no change log
file exists and the scope includes third-party cameras, the recorder performs a
best-effort reset — clearing `smartDetectTypes`, `objectTypes`, and
`smartDetectZones` to empty arrays for all third-party cameras.

**Example — enable change logging in the service file:**
```bash
ExecStart=/root/onvif_recorder \
  --detect_override --model_dir=/root/models \
  --change_log=/root/cam_changes.jsonl
```

**Example — rollback all changes and exit:**
```bash
/root/onvif_recorder --rollback=all --change_log=/root/cam_changes.jsonl
```

---

### Detection buffers

| Flag | Default | Description |
|------|---------|-------------|
| `--pre_buffer_sec` | `2` | How many seconds before the first detection event to mark as the start of a clip. |
| `--post_buffer_sec` | `2` | How many seconds after the last detection event to mark as the end of a clip. |

### Logging

| Flag | Default | Description |
|------|---------|-------------|
| `--verbose` | `false` | Enable informational logging: subscription established, events received, renewals. Without this flag only errors are logged. |
| `--event_log` | _(disabled)_ | Path to write each parsed ONVIF event as a JSON Lines entry. One line per event with topic, source, data, and timestamp. Useful for understanding what your cameras are reporting. |
| `--raw_log` | _(disabled)_ | Path to write every raw SOAP HTTP exchange as a JSON Lines entry. One line per request/response pair — these files are large. Used for deep diagnostics and bug reports. |

**Log levels** (controlled via `--verbose`):
- **Default (errors only):** Connection failures, database errors, and other problems that need attention.
- **Verbose (`--verbose`):** Everything above, plus subscription lifecycle (connected, renewed, disconnected) and event counts per poll cycle.

### Thumbnails (optional)

| Flag | Default | Description |
|------|---------|-------------|
| `--ubv_dir` | _(disabled)_ | Directory for per-camera UBV thumbnail files. Each camera gets its own file: `<dir>/<camera_ip>_thumbnails.ubv`. If not set, thumbnails are only written to the PostgreSQL `thumbnails` table. |
| `--model_dir` | _(disabled)_ | Directory containing `nanodet_m.param` and `nanodet_m.bin` for on-device NanoDet-M object detection. Required for `--detect` and `--detect_override`. |
| `--detect` | `false` | When the camera provides no ONVIF bounding box, run NanoDet-M to find the subject and crop the thumbnail. Falls back to the full uncropped image if detection finds nothing. Requires `--model_dir`. |
| `--detect_override` | `false` | Always run NanoDet-M for thumbnail cropping, ignoring any ONVIF bounding box the camera provides. Implies `--detect`. Requires `--model_dir`. |

**Thumbnail crop priority:**
1. `--detect_override` set → always use NanoDet-M
2. Camera provides an ONVIF bounding box → crop to that box
3. `--detect` set and no ONVIF box → run NanoDet-M, fall back to full image
4. Default (neither flag) → full uncropped image when no ONVIF box

---

## Performance (Dream Router)

The recorder uses negligible CPU at normal camera workloads:

### ONVIF event processing

| Load | CPU (single core) | Share of total CPU (4 cores) |
|------|-------------------|------------------------------|
| 60 ev/min (typical) | 0.036% of 1 core | **< 0.01%** |
| 2,714 ev/s (benchmark max) | 97.5% of 1 core | 24.4% |

### NanoDet-M object detection (optional)

NanoDet-M runs on one core using ARM64 NEON SIMD (compiled from source, no GPU):

| Metric | Value |
|--------|-------|
| Inference latency | ~158 ms / detection |
| Throughput | ~6 inferences / second |
| 1 detection/minute | 0.26% of 1 core ≈ **0.065% of total CPU** |
| 5 detections/minute | 1.3% of 1 core ≈ **0.33% of total CPU** |

NanoDet-M is safe to run on a Dream Router at typical home-camera workloads.
Only detections that reach the recorder trigger an inference — idle cameras
and cameras that send AI events (which already include a bounding box) add zero cost.

---

## Troubleshooting

**Check the log for errors:**
```bash
tail -f /var/log/onvif-recorder.log
```

**Enable verbose output** to see per-camera lifecycle events:
```bash
systemctl stop onvif-recorder
/root/onvif_recorder --verbose
```

**Camera not working?** Capture a raw diagnostic log and open a GitHub issue:
```bash
/root/onvif_recorder --verbose --raw_log=/tmp/onvif-raw.jsonl
# Let it run 60+ seconds (one full subscribe → pull → renew cycle), then Ctrl+C
# Attach /tmp/onvif-raw.jsonl to your issue
```

**Camera went offline and did not reconnect?** The recorder automatically retries.
After 3 consecutive failures it pauses for up to 1 hour, then resumes. If a camera
reboot takes longer than expected, restart the service:
```bash
systemctl restart onvif-recorder
```

---

## Building from source

### Prerequisites

- **Ubuntu 24.04** x86_64 build host (Ubuntu 22.04 also works; see note below)
- [Bazelisk](https://github.com/bazelbuild/bazelisk) installed at `~/.local/bin/bazel`
- Git with submodule support

### 1. Clone with submodules

```bash
git clone --recurse-submodules https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener.git
cd ubiquiti-protect-onvif-event-listener
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

### 2. Install Bazelisk

```bash
mkdir -p ~/.local/bin
curl -fsSL https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 \
  -o ~/.local/bin/bazel
chmod +x ~/.local/bin/bazel
```

### 3. Install apt dependencies

All C libraries (OpenSSL, libcurl, libxml2, libjpeg-turbo, libmicrohttpd,
libpq/libpgcommon/libpgport, GMP, Nettle, libtasn1) are built from source via
Bazel — either git submodules or HTTP archives. Only build tools are required
from apt:

```bash
sudo apt-get install -y \
  build-essential clang cmake ninja-build perl \
  python3-cpplint
```


### 4. Build

```bash
# x86_64 native binary
~/.local/bin/bazel build //:onvif_recorder

# ARM64 release binary — PGO + ThinLTO optimised (for Dream Router / Dream Machine)
scripts/bz build --config=arm64_release //:onvif_recorder

# Run all tests
~/.local/bin/bazel test //test:all

# Throughput benchmark
~/.local/bin/bazel run //test:bench_onvif_listener

# PGO + ThinLTO optimised build (x86)
~/.local/bin/bazel run //:pgo_bench_x86
```

The ARM64 build downloads its own sysroot automatically — no manual cross-toolchain
setup is required.

### Runtime dependencies (ARM64)

The ARM64 binary is almost entirely statically linked:

```
libm.so.6  libc.so.6  ld-linux-aarch64.so.1  libgcc_s.so.1
```

All other libraries (libcurl, OpenSSL, libxml2, libjpeg-turbo, GnuTLS, etc.) are
compiled in from the git submodules or the ARM64 sysroot.

### Runtime dependencies (x86_64)

The x86 binary links most libraries statically but keeps a few system libraries
dynamic (LDAP, LBER — standard packages present on any Ubuntu system):

```
libm.so.6  libc.so.6  ld-linux-x86-64.so.1  libgcc_s.so.1
libldap.so.2  liblber.so.2
```

---

## Project structure

```
onvif_listener.hpp/.cpp       — WS-PullPoint ONVIF subscription library
detection_recorder.hpp/.cpp   — Detection → PostgreSQL recorder
motion_poller.hpp/.cpp        — First-party camera motion → smart detect poller
camera_change_log.hpp/.cpp    — Cameras-table change log and rollback support
ubv_thumbnail.hpp/.cpp        — UBV container encode/decode (thumbnail storage)
unifi_camera_config.hpp/.cpp  — Load camera credentials from UniFi Protect DB
main.cpp                      — Binary entry point
bazel/
  pkg_config.bzl              — Repository rule: wraps host libs via pkg-config
  arm64_sysroot.bzl           — Repository rule: ARM64 sysroot + Clang toolchain
test/
  onvif_camera_emulator.hpp/.cpp  — libmicrohttpd fake camera base class
  camera_emulators.hpp/.cpp       — Camera 108/109 concrete emulators
  test_onvif_listener.cpp         — Listener integration test
  test_detection_recorder.cpp     — Detection recorder e2e test
  test_ubv_thumbnail.cpp          — UBV round-trip test
  bench_onvif_listener.cpp        — Single-core throughput benchmark
```
