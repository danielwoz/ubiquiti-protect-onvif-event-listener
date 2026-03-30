# ONVIF Event Recorder

Bridges **third-party ONVIF cameras** into UniFi Protect. It listens to ONVIF
WS-PullPoint event streams and writes detection events (person, vehicle, animal,
package) to the UniFi Protect PostgreSQL database so they appear natively in the
Protect UI — including timeline clips, thumbnails, smart detection search, and
security alarms.

> **Note:** This software is only needed for third-party cameras adopted into
> UniFi Protect via the ONVIF integration. Native UniFi cameras have built-in
> smart detection and do not require this tool.

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

**Generic motion events** (CellMotionDetector, VideoSource/MotionAlarm) carry no object class — the recorder uses `--default_object_type` for these. AI-specific events (Human/Vehicle detections) are never affected by `--default_object_type`.

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

| Load | CPU (single core) | Share of total CPU (4 cores) |
|------|-------------------|------------------------------|
| 60 ev/min (typical) | 0.036% of 1 core | **< 0.01%** |
| 2,714 ev/s (benchmark max) | 97.5% of 1 core | 24.4% |

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
detection_recorder.hpp/.cpp   — Detection → SQLite/PostgreSQL recorder
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
