# CLAUDE.md — Claude Code guide for this project

## Build commands

Use `scripts/bz` instead of `bazel` directly. The `bz` wrapper automatically assigns
a separate `--output_base` per `--config`, so switching configs never invalidates the
analysis cache of another config.

```bash
# Host (x86_64) binary  [cache: ~/.cache/bazel/x86]
scripts/bz build --config=x86 //:onvif_recorder

# ARM64 cross-compiled binary (for Dream Machine)  [cache: ~/.cache/bazel/arm64]
scripts/bz build --config=arm64 //:onvif_recorder

# All tests  [cache: ~/.cache/bazel/x86]
scripts/bz test --config=x86 //test:all

# Individual tests
scripts/bz run --config=x86 //test:test_detection_recorder
scripts/bz run --config=x86 //test:test_onvif_listener       # testdata JSONL passed automatically
scripts/bz run --config=x86 //test:test_ubv_thumbnail        # snapshot JPEGs passed automatically

# Manual inspection with a custom file
scripts/bz run --config=x86 //test:test_onvif_listener -- /path/to/other.jsonl
scripts/bz run --config=x86 //test:test_ubv_thumbnail  -- /path/to/file.ubv

# Throughput benchmark (single core, 50 000 events default)
scripts/bz run --config=x86 //test:bench_onvif_listener
scripts/bz run --config=x86 //test:bench_onvif_listener -- 100000   # custom event count

# JPEG crop benchmark (single core)
scripts/bz run --config=x86 //test:bench_jpeg_crop                  # uses security_cam_outdoor.jpg

# Object detection benchmark (single core, 100 iterations)
# Model files are downloaded automatically by Bazel via http_file rules.
scripts/bz run --config=x86 //test:bench_object_detect

# Sanitiser builds  [separate caches per sanitiser]
scripts/bz test --config=x86_asan //test:all   # AddressSanitiser
scripts/bz test --config=x86_tsan //test:all   # ThreadSanitiser
scripts/bz test --config=x86_msan //test:all   # MemorySanitiser (needs instrumented libc)

# PGO + ThinLTO optimised build (x86)
scripts/bz run --config=x86 //:pgo_bench_x86                   # baseline → instrument → profile → optimised
scripts/bz run --config=x86 //:pgo_bench_x86 -- 100000         # custom event count

# ARM64 release build (PGO + LTO, uses committed pgo_arm64.profdata)  [cache: ~/.cache/bazel/arm64_release]
scripts/bz build --config=arm64_release //:onvif_recorder

# Collect/refresh the ARM64 PGO profile (native profile via QEMU)
# Prerequisite: sudo apt-get install qemu-user-static
# Stages pgo_arm64.profdata automatically; commit it afterwards.
scripts/bz run --config=arm64 //:pgo_collect_arm64

# ARM64 benchmark under QEMU
scripts/bz run --config=arm64 //:pgo_bench_arm64
```

`scripts/bz` is a thin wrapper: it reads `--config=<name>` from args and injects
`--output_base=$HOME/.cache/bazel/<name>` as a Bazel startup option.  You can set
`BAZEL_CACHE_ROOT` to change the root directory.

Bazelisk is at `~/.local/bin/bazel` (auto-downloads Bazel 7.4.1 per `.bazelversion`).

## Code style

Google C++ Style Guide enforced via cpplint:

```bash
python3 -m cpplint <file>
```

Key rules in practice:
- K&R braces — `{` at end of previous line, never on its own line
- Access specifiers indented +1 space: ` public:`, ` protected:`, ` private:`
- Include order: matching `.h` → C system (`<foo.h>`) → C++ system → other
- `// NOLINT(runtime/int)` on libcurl lines that use `long` (required by the API)
- Line length limit: 100 (set in `CPPLINT.cfg`)

## Project structure

```
onvif_listener.hpp/.cpp       — WS-PullPoint ONVIF subscription library
                                  Parses tt:BoundingBox from ONVIF analytics events
detection_recorder.hpp/.cpp   — Detection → PostgreSQL recorder
                                  Crops thumbnails: ONVIF bbox → ML detect → smart crop
motion_poller.hpp/.cpp        — First-party camera motion → smart detect poller
                                  Polls events table, runs NanoDet-M on Protect thumbnails
camera_change_log.hpp/.cpp    — Cameras-table change log and rollback support
protect_ui_patch.hpp/.cpp     — Live-patch Protect UI alarm picker for third-party cameras
ubv_thumbnail.hpp/.cpp        — UBV container encode/decode (thumbnail storage)
jpeg_crop.hpp/.cpp            — JPEG decode/crop/re-encode via libjpeg
object_detect.hpp/.cpp        — NanoDet-M on-device object detection via NCNN
                                  Built with WITH_NCNN on both x86 and ARM64 (NEON SIMD)
unifi_camera_config.hpp/.cpp  — Load camera credentials from UniFi Protect DB
main.cpp                      — Binary entry point

.githooks/
  pre-push                    — shared pre-push hook (lint + x86 tests + Docker ARM64 build)
                                Activate with: bazel run //:install_hooks

third_party/
  ncnn.BUILD                  — filegroup for NCNN source archive
  BUILD.bazel                 — rules_foreign_cc cmake() target building libncnn.a

test/
  onvif_camera_emulator.hpp/.cpp  — libmicrohttpd fake camera base class
  camera_emulators.hpp/.cpp       — Camera 108 / 109 concrete emulators
  test_onvif_listener.cpp         — Listener integration test
  test_detection_recorder.cpp     — Detection recorder e2e test
  test_ubv_thumbnail.cpp          — UBV round-trip test
  bench_onvif_listener.cpp        — ONVIF parsing throughput benchmark
  bench_jpeg_crop.cpp             — JPEG crop throughput benchmark
  bench_object_detect.cpp         — NanoDet-M inference latency benchmark
  testdata/
    snapshot_108.jpg              — Real JPEG from cam 192.168.1.108
    snapshot_109.jpg              — Real JPEG from cam 192.168.1.109
    security_cam_outdoor.jpg      — 1562×1020 outdoor CCTV (public domain, Wikimedia)
    security_cam_person.jpg       — 2750×4080 indoor CCTV person (public domain)
    security_cam_vehicle.jpg      — 1050×656 outdoor CCTV vehicle (public domain)
```

## Database backend

`DetectionRecorder` uses **PostgreSQL** (the UniFi Protect database). Thumbnails are
inserted directly into the `thumbnails` table as `bytea` with a 24-char hex `id` so
UniFi Protect's UI routes them correctly (IDs of length ≠ 24 go to msp TCP and fail).

Optionally, per-camera UBV thumbnail files are also written to `ONVIF_UBV_DIR` if set.

## Key architectural notes

- One thread per camera in `OnvifListener`; `EventCallback` is called from camera
  threads and must be thread-safe.
- `CameraConfig::max_consecutive_failures` (default 0 = unlimited) controls give-up
  behaviour; `main.cpp` sets it to 5 for production. Tests leave it at 0 because
  some emulated cameras replay startup-failure responses before succeeding.
- `CameraConfig::id` and `mac` are populated from the UniFi Protect DB and used by
  the PostgreSQL backend to set `cameraId` and generate thumbnail IDs.
- Detection mapping: ONVIF `"Human"` → `"person"`, `"Vehicle"` → `"vehicle"`.
- `"end"` must be double-quoted in SQL (reserved word).
- Thumbnail crop priority: ONVIF `tt:BoundingBox` (if w>0 && h>0) → ML detector
  (`ObjectDetector::detect()`) → smart_crop heuristic (square at 60% vertical centre).
- `ObjectDetector` uses NanoDet-M (NCNN) on both x86 and ARM64.  NCNN is built
  from source via `rules_foreign_cc` cmake() for both architectures; the ARM64
  target (`//third_party:ncnn_arm64`) sets `CMAKE_SYSTEM_NAME=Linux`,
  `CMAKE_SYSTEM_PROCESSOR=aarch64`, and `CMAKE_CROSSCOMPILING=TRUE` so CMake
  enters cross-compilation mode and enables NEON SIMD automatically.  Pass
  `--model_dir` with `--detect` (or `--detect_override`) to enable on-device
  detection.
- Thumbnail crop modes (default → ONVIF bbox only; no bbox → full image stored uncropped):
  - default: ONVIF bbox → crop; no bbox → full image
  - `--detect`: ONVIF bbox → crop; no bbox → NanoDet-M → full image
  - `--detect_override`: always NanoDet-M → full image (ONVIF bbox discarded)
- NCNN is built from source via `rules_foreign_cc` cmake(); model files are downloaded
  by Bazel `http_file` rules (`nanodet_m_param`, `nanodet_m_bin`).

## Pre-push checklist

Run these before every `git push` to keep the repo green:

```bash
# 1. Lint all source files (must be zero errors)
python3 -m cpplint *.cpp *.hpp test/*.cpp test/*.hpp

# 2. All tests pass (Bazel-cached; fast on repeat runs)
scripts/bz test --config=x86 //test:all

# 3. ARM64 Docker build (validates sysroot/toolchain on ubuntu:24.04 + clang-18)
./build-in-docker.sh --arm64
```

> **Note:** these steps are a manual checklist for Claude to follow in conversation.
> To enforce them automatically on every push, install the shared git hooks (tracked
> in `.githooks/`):
>
> ```bash
> bazel run //:install_hooks
> ```
>
> This runs `git config core.hooksPath .githooks` so Git uses the `.githooks/pre-push`
> file that is checked into the repository. Anyone who clones the repo runs the same
> one-time command to activate the hooks.
> The hook skips the Docker ARM64 build automatically when Docker is not available.

> **IMPORTANT:** Never use `git push --no-verify`. The pre-push hook exists to prevent
> broken commits reaching the repository. If the hook fails, fix the underlying issue
> first. If GitHub's SSH connection times out while the hook is running, investigate
> and resolve the root cause — do not bypass the hook.

## Release checklist

When creating a new GitHub release:

```bash
# 1. Tag the release commit
git tag v<X.Y.Z>
git push origin v<X.Y.Z>

# 2. Build the ARM64 release binary (PGO + ThinLTO)
scripts/bz build --config=arm64_release //:onvif_recorder

# 3. Create the release with assets
gh release create v<X.Y.Z> \
  --title "v<X.Y.Z>" \
  --notes "release notes here" \
  ~/.cache/bazel/arm64_release/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder#onvif_recorder_arm64 \
  onvif-recorder.service
```

**Required release assets:**
- `onvif_recorder_arm64` — ARM64 release binary built at the tagged commit
- `onvif-recorder.service` — systemd service file

## Command-line flags (runtime)

All configuration is now via `absl::flags`. Pass `--help` for the full list.

| Flag | Default | Description |
|---|---|---|
| `--db_conn` | `host=/run/postgresql port=5433 dbname=unifi-protect user=postgres` | libpq conninfo for the UniFi Protect database |
| `--db_host` | _(empty = Unix socket)_ | Override PostgreSQL host for camera config loading |
| `--ubv_dir` | _(auto-detected)_ | Directory for per-camera UBV thumbnail files. Auto-detected on Dream Routers (`/srv/unifi-protect/video`). |
| `--pre_buffer_sec` | `2` | Seconds to buffer before the first detection event |
| `--post_buffer_sec` | `2` | Seconds to buffer after the last detection event |
| `--verbose` | `false` | Set absl log level to INFO (lifecycle, events, renewals). Default: ERROR only |
| `--event_log` | _(disabled)_ | Path for parsed-event JSON Lines log; one line per ONVIF event |
| `--raw_log` | _(disabled)_ | Path for raw SOAP exchange JSON Lines log; one line per HTTP request/response |
| `--model_dir` | `/root/models` | Directory containing `nanodet_m.param` and `nanodet_m.bin`. Models are downloaded automatically if not present. |
| `--detect` | `true` | Enable NanoDet-M as fallback when the camera provides no ONVIF bbox |
| `--detect_override` | `false` | Always run NanoDet-M, ignoring the ONVIF bbox entirely (implies `--detect`) |
| `--coalesce_window_sec` | `30` | Merge consecutive detections from the same camera into one event if the new detection starts within this many seconds of the previous one ending. Set to 0 to disable. |
| `--max_events_per_hour` | `10` | Maximum new detection events per camera per hour. Events beyond this limit are dropped. Set to 0 for unlimited. |
| `--coalesce_history` | `true` | On startup, scan the last `--coalesce_history_days` days of events and merge consecutive detections from the same third-party camera within `--coalesce_window_sec`. Only third-party (ONVIF) cameras are affected. |
| `--coalesce_history_days` | `30` | Number of days to look back when `--coalesce_history` is set. |
| `--first_party_cameras` | _(empty)_ | Comma-separated camera IDs of first-party cameras to enable smart detection flags for in the cameras table. |
| `--poll_interval_sec` | `10` | Seconds between motion-event poll cycles for first-party cameras. |
| `--change_log` | _(empty)_ | Path for cameras-table change log (JSON Lines). Records old/new values for rollback. |
| `--rollback` | _(empty)_ | Undo cameras-table changes and exit. Values: `third_party`, `first_party`, `all`. |
| `--protect_url` | `http://localhost:7080` | Base URL for the local Protect API used to trigger automations on smart detection events. |
| `--protect_user_id` | _(auto-discovered)_ | X-UserId header for Protect API auth bypass. Auto-discovered from unifi-core DB on first run and cached to `/root/.config/onvif-recorder-api-key`. Pass explicitly to override. |
| `--patch_alarm_picker` | `true` | Live-patch the Protect UI to allow third-party cameras in the alarm creation picker. Re-applied on every startup so it survives firmware updates. |

Logging uses absl/log. `--verbose` calls `absl::SetMinLogLevel(kInfo)`; default is `kError`.
`enable_verbose_logging()` has been removed from `OnvifListener`; set log level via absl before calling `run()`.

`ubv_extract` accepts `--db_host` (default `127.0.0.1`) to override the Protect DB host.
`gen_examples` accepts `--model_dir` for the NanoDet-M model path.

## NanoDet-M object detection model

NanoDet-M is used for thumbnail subject cropping when the camera does not provide
an ONVIF bounding box.  It is enabled with `--detect` (fallback) or
`--detect_override` (always run), both of which require `--model_dir`.

### Downloading the model files

The model files are hosted in the
[nihui/ncnn-assets](https://github.com/nihui/ncnn-assets) repository.
Bazel downloads them automatically when running `bench_object_detect` or any
target that depends on `object_detect`.  To fetch them manually:

```bash
# These are the exact URLs used by the Bazel http_file rules in WORKSPACE.
curl -L -o nanodet_m.param \
  https://github.com/nihui/ncnn-assets/raw/refs/heads/master/models/nanodet_m.param
curl -L -o nanodet_m.bin \
  https://github.com/nihui/ncnn-assets/raw/refs/heads/master/models/nanodet_m.bin
```

Both files must live in the same directory, which is passed to `--model_dir`.

### Running on an x86 host

```bash
# Place model files in a local directory, e.g. ~/models/
mkdir -p ~/models
cp nanodet_m.param nanodet_m.bin ~/models/

# Run with NanoDet-M as a fallback (fires when the camera has no bbox)
./bazel-bin/onvif_recorder --detect --model_dir=$HOME/models

# Run with NanoDet-M always overriding the camera bbox
./bazel-bin/onvif_recorder --detect_override --model_dir=$HOME/models
```

### Deploying the model to a Dream Router / NVR

NCNN is now built from source for ARM64 (with NEON SIMD).  After copying the
binary to the router, copy the model files too:

```bash
# Copy model files to the router
ssh root@<router-ip> "mkdir -p /root/models"
scp nanodet_m.param nanodet_m.bin root@<router-ip>:/root/models/
```

Then update the service file to pass `--model_dir` (and `--detect` or
`--detect_override`) to the recorder:

```ini
[Service]
ExecStart=/root/onvif_recorder --detect --model_dir=/root/models
```

Reload and restart after editing the service file:

```bash
ssh root@<router-ip> "systemctl daemon-reload && systemctl restart onvif-recorder"
```

## Deploying to a Dream Router / NVR

```bash
# 1. Cross-compile for ARM64 (PGO + ThinLTO optimised release)
scripts/bz build --config=arm64_release //:onvif_recorder

# 2. Copy binary to router (replace <router-ip> with your device's IP)
scp bazel-bin/onvif_recorder root@<router-ip>:/root/onvif_recorder

# 3. Copy (or update) the systemd service file
scp onvif-recorder.service root@<router-ip>:/etc/systemd/system/onvif-recorder.service

# 4. Reload systemd and restart the service on the router
ssh root@<router-ip> "systemctl daemon-reload && systemctl restart onvif-recorder"

# 5. Verify it is running
ssh root@<router-ip> "systemctl status onvif-recorder"
```

The binary lives at `/root/onvif_recorder` and the service file at
`/etc/systemd/system/onvif-recorder.service` on UniFi Dream Routers and NVRs.
