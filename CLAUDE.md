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
detection_recorder.hpp/.cpp   — Detection → SQLite/PostgreSQL recorder
                                  Crops thumbnails: ONVIF bbox → ML detect → smart crop
ubv_thumbnail.hpp/.cpp        — UBV container encode/decode (thumbnail storage)
jpeg_crop.hpp/.cpp            — JPEG decode/crop/re-encode via libjpeg
object_detect.hpp/.cpp        — NanoDet-M on-device object detection via NCNN
                                  Guarded by WITH_NCNN; stub returns nullopt without NCNN
unifi_camera_config.hpp/.cpp  — Load camera credentials from UniFi Protect DB
main.cpp                      — Binary entry point

.githooks/
  pre-push                    — shared pre-push hook (lint + tests + PGO bench)
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
- `ObjectDetector` uses NanoDet-M (NCNN) on host; ARM64 builds compile without
  `WITH_NCNN` and always return `nullopt` (smart_crop fallback).  Pass `--model_dir`
  with `--detect` (or `--detect_override`) to enable on-device detection.
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

> **IMPORTANT:** Never use `git push --no-verify`. The pre-push hook exists to prevent
> broken commits reaching the repository. If the hook fails, fix the underlying issue
> first. If GitHub's SSH connection times out while the hook is running, investigate
> and resolve the root cause — do not bypass the hook.

## Command-line flags (runtime)

All configuration is now via `absl::flags`. Pass `--help` for the full list.

| Flag | Default | Description |
|---|---|---|
| `--db_conn` | `host=/run/postgresql port=5433 dbname=unifi-protect user=postgres` | libpq conninfo for the UniFi Protect database |
| `--db_host` | _(empty = Unix socket)_ | Override PostgreSQL host for camera config loading |
| `--ubv_dir` | _(empty)_ | Directory for per-camera UBV thumbnail files (optional) |
| `--pre_buffer_sec` | `2` | Seconds to buffer before the first detection event |
| `--post_buffer_sec` | `2` | Seconds to buffer after the last detection event |
| `--verbose` | `false` | Set absl log level to INFO (lifecycle, events, renewals). Default: ERROR only |
| `--event_log` | _(disabled)_ | Path for parsed-event JSON Lines log; one line per ONVIF event |
| `--raw_log` | _(disabled)_ | Path for raw SOAP exchange JSON Lines log; one line per HTTP request/response |
| `--model_dir` | _(empty)_ | Directory containing `nanodet_m.param` and `nanodet_m.bin` |
| `--detect` | `false` | Enable NanoDet-M as fallback when the camera provides no ONVIF bbox (requires `--model_dir`) |
| `--detect_override` | `false` | Always run NanoDet-M, ignoring the ONVIF bbox entirely (implies `--detect`) |
| `--coalesce_window_sec` | `30` | Merge consecutive detections from the same camera into one event if the new detection starts within this many seconds of the previous one ending. Set to 0 to disable. |
| `--max_events_per_hour` | `10` | Maximum new detection events per camera per hour. Events beyond this limit are dropped. Set to 0 for unlimited. |
| `--coalesce_history` | `false` | On startup, scan the last `--coalesce_history_days` days of events and merge consecutive detections within `--coalesce_window_sec`. Applies to all cameras. |
| `--coalesce_history_days` | `30` | Number of days to look back when `--coalesce_history` is set. |

Logging uses absl/log. `--verbose` calls `absl::SetMinLogLevel(kInfo)`; default is `kError`.
`enable_verbose_logging()` has been removed from `OnvifListener`; set log level via absl before calling `run()`.

`ubv_extract` accepts `--db_host` (default `127.0.0.1`) to override the Protect DB host.
`gen_examples` accepts `--model_dir` for the NanoDet-M model path.

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
