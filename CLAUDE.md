# CLAUDE.md — Claude Code guide for this project

## Build commands

```bash
# Host (x86_64) binary
bazel build //:onvif_recorder

# ARM64 cross-compiled binary (for Dream Machine)
bazel build --config=arm64 //:onvif_recorder

# All tests
bazel test //test:all

# Individual tests
bazel run //test:test_detection_recorder
bazel run //test:test_onvif_listener       # testdata JSONL passed automatically
bazel run //test:test_ubv_thumbnail        # snapshot JPEGs passed automatically

# Manual inspection with a custom file
bazel run //test:test_onvif_listener -- /path/to/other.jsonl
bazel run //test:test_ubv_thumbnail  -- /path/to/file.ubv

# Throughput benchmark (single core, 50 000 events default)
bazel run //test:bench_onvif_listener
bazel run //test:bench_onvif_listener -- 100000   # custom event count

# JPEG crop benchmark (single core)
bazel run //test:bench_jpeg_crop                  # uses security_cam_outdoor.jpg

# Object detection benchmark (single core, 100 iterations)
# Model files are downloaded automatically by Bazel via http_file rules.
bazel run //test:bench_object_detect

# PGO + ThinLTO optimised build (x86)
make pgo-bench-x86                   # baseline → instrument → profile → optimised
make pgo-bench-x86 PGO_EVENTS=100000

# ARM64 benchmark under QEMU (cross-PGO reuses x86 profile)
# Prerequisite: sudo apt-get install qemu-user-static
# Prerequisite: run pgo-bench-x86 first to generate pgo.profdata
make pgo-bench-arm64
```

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
  - `--detect`: ONVIF bbox → crop; no bbox → NanoDet-M → smart_crop
  - `--detect_override`: always NanoDet-M → smart_crop (ONVIF bbox discarded)
- NCNN is built from source via `rules_foreign_cc` cmake(); model files are downloaded
  by Bazel `http_file` rules (`nanodet_m_param`, `nanodet_m_bin`).

## Pre-push checklist

Run these before every `git push` to keep the repo green:

```bash
# 1. Lint all source files (must be zero errors)
python3 -m cpplint *.cpp *.hpp test/*.cpp test/*.hpp

# 2. All tests pass
bazel test //test:all

# 3. Regenerate PGO profiles (keeps the optimised binary current)
make pgo-bench-x86
```

> **Note:** these steps are a manual checklist for Claude to follow in conversation.
> To enforce them automatically on every push, configure a git pre-push hook:
>
> ```bash
> cat > .git/hooks/pre-push << 'EOF'
> #!/bin/bash
> set -e
> python3 -m cpplint *.cpp *.hpp test/*.cpp test/*.hpp
> ~/.local/bin/bazel test //test:all
> make pgo-bench-x86
> EOF
> chmod +x .git/hooks/pre-push
> ```

## Command-line flags (runtime)

All configuration is now via `absl::flags`. Pass `--help` for the full list.

| Flag | Default | Description |
|---|---|---|
| `--db_conn` | `host=/run/postgresql port=5433 dbname=unifi-protect user=postgres` | libpq conninfo for the UniFi Protect database |
| `--db_host` | _(empty = Unix socket)_ | Override PostgreSQL host for camera config loading |
| `--ubv_dir` | _(empty)_ | Directory for per-camera UBV thumbnail files (optional) |
| `--pre_buffer_sec` | `2` | Seconds to buffer before the first detection event |
| `--post_buffer_sec` | `2` | Seconds to buffer after the last detection event |
| `--verbose` | `false` | Enable verbose logging (lifecycle, events, renewals) |
| `--model_dir` | _(empty)_ | Directory containing `nanodet_m.param` and `nanodet_m.bin` |
| `--detect` | `false` | Enable NanoDet-M as fallback when the camera provides no ONVIF bbox (requires `--model_dir`) |
| `--detect_override` | `false` | Always run NanoDet-M, ignoring the ONVIF bbox entirely (implies `--detect`) |

`ubv_extract` accepts `--db_host` (default `127.0.0.1`) to override the Protect DB host.
`gen_examples` accepts `--model_dir` for the NanoDet-M model path.
