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
src/
  onvif_listener.hpp/.cpp       — WS-PullPoint ONVIF subscription library
                                    Parses tt:BoundingBox from ONVIF analytics events
  detection_recorder.hpp/.cpp   — Detection → PostgreSQL recorder
                                    Crops thumbnails: ONVIF bbox → ML detect → smart crop
  motion_poller.hpp/.cpp        — First-party camera motion → smart detect poller
                                    Polls events table, runs NanoDet-M on Protect thumbnails
  alarm_notifier.hpp/.cpp       — Protect API alarm notifier (triggers automations on smart detect)
  msr_client.hpp/.cpp           — UniFi MSR gRPC client (forwards thumbnails via RecordingAPI.StoreSnapshots
                                    over HTTP/2 cleartext on 127.0.0.1:7700)
  camera_change_log.hpp/.cpp    — Cameras-table change log and rollback support
  protect_ui_patch.hpp/.cpp     — Live-patch Protect UI alarm picker for third-party cameras
  ubv_thumbnail.hpp/.cpp        — UBV container encode/decode (thumbnail storage)
  jpeg_crop.hpp/.cpp            — JPEG decode/crop/re-encode via libjpeg
  object_detect.hpp/.cpp        — NanoDet-M on-device object detection via NCNN
                                    Built with WITH_NCNN on both x86 and ARM64 (NEON SIMD)
  unifi_camera_config.hpp/.cpp  — Load camera credentials from UniFi Protect DB
  event_recorder.hpp/.cpp       — Thread-safe JSON Lines writer for parsed ONVIF events
  util.hpp/.cpp                 — Shared UUID, timestamp, JSON string helpers
  main.cpp                      — Binary entry point
  ubv_extract.cpp               — Standalone UBV thumbnail extraction tool

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
  test_camera_change_log.cpp     — Change log unit tests
  test_protect_ui_patch.cpp       — UI patch apply/revert tests
  test_unifi_camera_config.cpp   — DB connection string / JSON / PG array helper tests
  test_motion_poller.cpp          — Smart detect type / SDR payload helper tests
  test_msr_client.cpp             — MSR gRPC wire-format encode/decode tests
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
  behaviour; `main.cpp` sets it to 3 for production. Tests leave it at 0 because
  some emulated cameras replay startup-failure responses before succeeding.
- Reconnect backoff after a failure run is bounded exponential:
  `retry_backoff_base_sec` (30) doubling to `retry_backoff_max_sec` (300), reset
  to base on the next successful subscription. It previously slept out the
  remainder of a 1 h window measured from the first failure of the streak, which
  turned a ~30 s blip into a ~59 min outage for that camera.
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
python3 -m cpplint src/*.cpp src/*.hpp test/*.cpp test/*.hpp

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

## Pre-release verification on a live router

> **MANDATORY before every `git push origin v<X.Y.Z>`.** Every prior breakage
> caught only after stable promotion (v1.4.3 nginx-injection nesting, v1.4.4
> `--only-upgrade` dead-end, v1.4.7 CSRF 403) would have surfaced if this
> runbook had been followed first. A green pass on steps 3–9 below is the
> precondition for tagging.

The runbook builds a `.deb` locally (without touching apt suites or creating
a release), deploys to the developer's UDM at `192.168.1.1` via passwordless
`ssh root@…` (configured per the project's standing setup), exercises every
in-process feature added since the last verified tag, soaks for an hour to
catch latent crash/leak/silent-stall regressions, and proves the live
config-edit-restart loop end-to-end.

Substitute the candidate version (e.g. `1.4.8`) for every `1.4.7` reference.
Total wall-clock budget: ~90 minutes (~5 min build + ~5 min deploy/smoke +
60 min soak + ~5 min config flip + final health check).

### 0. Pre-flight

```bash
git status                                          # clean working tree expected
scripts/bz test --config=x86 //test:all             # all tests green
python3 -m cpplint src/*.cpp src/*.hpp test/*.cpp test/*.hpp
```

If anything is uncommitted that should ship in the candidate, commit + push
first so the binary we deploy matches `origin/main`.

### 1. Build the .deb locally with explicit version

```bash
scripts/bz build --config=arm64_release //:onvif_recorder
scripts/build-deb.sh --arch=arm64 --version=1.4.7
ls -lh dist/onvif-recorder_1.4.7_arm64.deb
```

`--version=` overrides the `git describe` derivation so we get a clean
`1.4.7` package even when the working tree is past the v1.4.6 tag and not
yet tagged for the candidate. The `arm64_release` build is PGO + ThinLTO.

### 2. Deploy to the router

```bash
scp dist/onvif-recorder_1.4.7_arm64.deb root@192.168.1.1:/tmp/
ssh root@192.168.1.1 "dpkg -i /tmp/onvif-recorder_1.4.7_arm64.deb"
```

Use `dpkg -i` (not `apt-get install`) so apt's downgrade-protection rule
doesn't refuse to install when the version string is below the current
remote stable.

### 3. Service-level smoke

```bash
ssh root@192.168.1.1 '
  systemctl is-active onvif-recorder &&
  dpkg-query -W -f="installed: ${Version}\n" onvif-recorder &&
  ps -fp $(pgrep -x onvif-recorder) | tail -1 &&
  nginx -t 2>&1 | tail -2
'
```

Pass: `active`, `installed: 1.4.7`, the process line shows the
expected flags from `/etc/default/onvif-recorder.local`, and `nginx -t`
reports `configuration file … syntax is ok`.

### 4. Endpoint smoke (loopback only — no UniFi auth in shell)

```bash
ssh root@192.168.1.1 '
  echo "--- /api/status ---"      ; curl -s http://127.0.0.1:7891/api/status | head -c 400; echo
  echo "--- /api/camera_health ---"; curl -s http://127.0.0.1:7891/api/camera_health | head -c 600; echo
  echo "--- /api/recent_events ---"; curl -s http://127.0.0.1:7891/api/recent_events | head -c 600; echo
  echo "--- /api/config ---"      ; curl -s http://127.0.0.1:7891/api/config | head -c 600; echo
  echo "--- HEAD admin ---"       ; curl -sI http://127.0.0.1:7891/ | head -1
  echo "--- log viewer ---"        ; curl -sI http://127.0.0.1:7890/ | head -1
'
```

Pass: every endpoint returns `200 OK` with valid JSON. HEAD on admin
returns `200` (regression check for the v1.4.6 admin HEAD-as-GET fix).

### 5. Diagnostic dump end-to-end (also exercises the sanitiser)

```bash
ssh root@192.168.1.1 '
  curl -s -o /tmp/dump.tgz http://127.0.0.1:7891/api/diagnostic_dump &&
  ls -lh /tmp/dump.tgz &&
  rm -rf /tmp/dump.x && mkdir /tmp/dump.x && tar -xzf /tmp/dump.tgz -C /tmp/dump.x &&
  ls /tmp/dump.x &&
  echo "--- IPs in journal (must all be 1.x.x.x not 192.168.x.x) ---" &&
  (grep -E "192\.168\.[0-9]+\.[0-9]+" /tmp/dump.x/journal.log | head -3 || echo "(none — sanitised)") &&
  echo "--- credentials/tokens in journal (count must be 0) ---" &&
  grep -ciE "password=[^[]|wsse:Password>[^[]" /tmp/dump.x/journal.log
'
```

Pass: archive contains `config.json`, `status.json`, `camera_health.json`,
`journal.log`, `dpkg.txt`, `system.txt`, `SANITIZED.txt`. No `192.168.*`
IPs in journal (all remapped to `1.x.x.x`). Credential count = 0.

### 6. Camera-health pill correctness

```bash
ssh root@192.168.1.1 '
  curl -s http://127.0.0.1:7891/api/camera_health | python3 -m json.tool
'
```

Pass: each adopted camera appears once with `is_third_party` set
correctly. `events_1h` is non-zero on at least one camera that has been
triggered in the last hour.

### 7. Soak — 1 hour, then re-verify event flow + thumbnails

```bash
ssh root@192.168.1.1 'logger -t onvif-soak "soak start v1.4.7 $(date -Is)"'
sleep 3600
ssh root@192.168.1.1 '
  echo "--- L1 heartbeats (expect at least one per active camera) ---" &&
  journalctl -u onvif-recorder --since "1 hour ago" --no-pager |
    grep -E "alive: events_recv=" | head &&
  echo "--- L2 hourly aggregate (expect one line) ---" &&
  journalctl -u onvif-recorder --since "1 hour ago" --no-pager |
    grep -E "\[recorder\] last 1h:" | head &&
  echo "--- recent third-party events ---" &&
  psql -h /run/postgresql -p 5433 -U postgres unifi-protect -c "
    SELECT type, count(*) FROM events
    WHERE start > (extract(epoch from now())*1000 - 3600000)::bigint
      AND \"cameraId\" IN (SELECT id FROM cameras WHERE \"isThirdPartyCamera\"=true)
    GROUP BY type ORDER BY 2 DESC;" &&
  echo "--- thumbnails written for those events ---" &&
  psql -h /run/postgresql -p 5433 -U postgres unifi-protect -c "
    SELECT count(*) AS thumbs_in_db_last_hour FROM thumbnails t
    JOIN events e ON e.\"thumbnailId\" = t.id
    WHERE e.start > (extract(epoch from now())*1000 - 3600000)::bigint;" &&
  echo "--- MSR-stored thumbnails in last hour ---" &&
  journalctl -u onvif-recorder --since "1 hour ago" --no-pager |
    grep -c "MSR stored snapshot"
'
```

Pass: at least one L1 heartbeat per active camera (12 in 1h at the 60s
cadence is the floor); exactly one L2 aggregate (since the hourly window
just rolled over); at least one third-party event in the last hour; thumb
table has rows for events that fell on the DB path (or `MSR stored` log
lines for MSR-routed events).

### 8. Live config edit — flip detect_override on, verify restart + behaviour

```bash
ssh root@192.168.1.1 '
  PID_BEFORE=$(pgrep -x onvif-recorder) &&
  curl -s -X POST -H "Content-Type: application/json" \
       -d "{\"detect_override\":\"true\"}" \
       http://127.0.0.1:7891/api/config &&
  echo &&
  for i in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    PID_NOW=$(pgrep -x onvif-recorder)
    [ -n "$PID_NOW" ] && [ "$PID_NOW" != "$PID_BEFORE" ] && break
  done &&
  echo "--- PID changed from $PID_BEFORE to $PID_NOW ---" &&
  echo "--- config.json on disk ---" &&
  cat /etc/onvif-recorder/config.json &&
  echo "--- new process detect-mode log ---" &&
  journalctl -u onvif-recorder --since "30 seconds ago" --no-pager |
    grep -E "\[detect\] (override|fallback) mode" | tail -2
'
```

Pass: file contains `"detect_override":"true"`; PID changes; new run logs
`[detect] override mode: NanoDet-M from /usr/share/onvif-recorder/models`.

Then revert:

```bash
ssh root@192.168.1.1 '
  PID_BEFORE=$(pgrep -x onvif-recorder) &&
  curl -s -X POST -H "Content-Type: application/json" \
       -d "{\"detect_override\":\"\"}" \
       http://127.0.0.1:7891/api/config &&
  for i in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    PID_NOW=$(pgrep -x onvif-recorder)
    [ -n "$PID_NOW" ] && [ "$PID_NOW" != "$PID_BEFORE" ] && break
  done &&
  echo "PID $PID_BEFORE -> $PID_NOW" &&
  journalctl -u onvif-recorder --since "30 seconds ago" --no-pager |
    grep -E "\[detect\] (override|fallback) mode" | tail -1
'
```

Pass: `[detect] fallback mode: NanoDet-M …` (override cleared and the
unit-file flag is back in effect).

### 9. Final health snapshot

```bash
ssh root@192.168.1.1 '
  systemctl is-active onvif-recorder &&
  journalctl -u onvif-recorder --since "1 hour ago" --no-pager |
    grep -ciE "error|warn|fatal"
'
```

Pass: `active`, error/warn count is bounded (≲ same as before deploy +
small margin for transient camera errors).

### 10. Tag-and-promote (only after all the above pass)

Proceed to the *Release checklist* section below. **Do not run that
checklist if any of steps 3–9 failed** — fix root cause, rebuild,
redeploy, re-soak.

## Release checklist

> **NON-NEGOTIABLE for every release.** A release without release notes and
> both binary + service-file assets is a broken release. `git tag` alone is
> NOT a release — it must be followed by `gh release create` with `--notes`
> and both assets in the same invocation, then verified via `gh release view`.
> If `gh` is unauthenticated, stop and re-authenticate (`gh auth login -h
> github.com`) — never publish a release without the assets and notes.

### Version-number convention: even vs odd patch

The patch-level version number signals whether the build was published or
not:

- **Even patch numbers (`v1.6.0`, `v1.6.2`, `v1.6.4`, …) are GitHub
  releases.** They have a `v*` tag, a `gh release` with notes, and both
  `.deb` + `.exe` assets attached. They get promoted through apt suites
  (`early-access` → `rc` → `stable`).
- **Odd patch numbers (`1.6.1-dev*`, `1.6.3-dev*`, `1.6.5-dev*`, …) are
  local-only dev builds.** They never get tagged, never get a GitHub
  release, never go into the apt repo. They exist purely to deploy
  in-flight code to the developer's own router for soak-testing.

In practice: while iterating, ship as `1.6.<odd>-dev<n>` (`1.6.3-dev1`,
`1.6.3-dev2`, …). When ready to release, **bump to the next even** —
`1.6.4` — and run the release checklist below. Do *not* publish odd
versions to GitHub; they're scratch space.

### Required release assets

Every `v*` release MUST ship these two assets, and only these two:

- `onvif-recorder_<ver>_arm64.deb` — Debian package (ARM64). Built via
  `scripts/build-deb.sh --arch=arm64` or `./build-in-docker.sh --deb`.
  Uploaded automatically by `.github/workflows/release-deb.yml`, which
  also publishes it into the `early-access` suite of the gh-pages apt
  repo.
- `OnvifRecorderInstaller-v<ver>.exe` — Windows installer (self-contained
  win-x64 single-file .NET 8 WPF exe). Uploaded automatically by
  `.github/workflows/release-windows.yml` (runs on `windows-2022`).

Both workflows fire on every `v*` tag push, so a correct release is:
push the tag, wait for both workflows to finish, then verify both
assets landed via `gh release view v<X.Y.Z>`.

The raw ARM64 binary (`onvif_recorder_arm64`) and the bare
`onvif-recorder.service` are NO LONGER release assets — the `.deb` is
the supported distribution path and bundles both.

### Uninstall behaviour (--rollback)

`debian/prerm` runs `/usr/bin/onvif-recorder --rollback=all` on
`apt-get remove` (but not on upgrade) before the binary is deleted, so
the package cleans up after itself:

- reverts the nginx `/onvif/events/log` and `/onvif/admin/` location
  blocks (from `protect_ui_patch::revert_nginx_{log,admin}_proxy`);
- restores Protect's `swai.js` from the `.bak` written at patch time
  (from `protect_ui_patch::revert_alarm_picker`);
- rolls back cameras-table changes recorded in `--change_log`.

`apt-get purge` additionally wipes `/etc/onvif-recorder`,
`/var/lib/onvif-recorder`, the systemd timer drop-ins, and the
apt source.

### Firmware survivability (persistent recovery layer)

UDM firmware upgrades replace the squashfs lower layer of the
overlayfs root, which wipes everything under `/usr/`, `/lib/`, and
parts of `/etc/` (notably `/etc/apt/sources.list.d/`). They preserve
`/data/`, `/etc/cron.d/`, and `/etc/systemd/system/`. Without a
recovery layer the package vanishes after every firmware upgrade and
the user has to re-run the installer manually — this is the root
cause of issue #30.

To make survival automatic, **`gh-pages/install.sh` deploys a small
recovery layer outside the .deb** (so it survives both firmware
upgrades and `apt purge`):

| Path | Role |
|------|------|
| `/data/onvif-recorder/install.sh` | Self-copy of the published installer (so boot-restore doesn't need network access to gh-pages just to find the script). |
| `/data/onvif-recorder/boot-restore.sh` | Wakes on boot via cron; if the .deb is missing, restores config from `backups/` and runs `install.sh`. |
| `/data/onvif-recorder/backup.sh` | Tars `/etc/onvif-recorder`, `/etc/default/onvif-recorder*`, and `/var/lib/onvif-recorder` to `backups/config-current.tar.gz`. |
| `/data/onvif-recorder/backups/config-current.tar.gz` | Latest config snapshot. Used by `boot-restore.sh` and re-built daily. |
| `/data/onvif-recorder/backups/config-YYYY-MM-DD.tar.gz` | Weekly snapshots (Sundays), trimmed to last 4. |
| `/data/onvif-recorder/.autoupdate-consent` | `true` or `false`. Controls whether `boot-restore.sh` actually re-installs. Regenerated from `config.json`'s `autoupdate_enabled` on every install. |
| `/etc/cron.d/onvif-recorder-boot-restore` | `@reboot root /data/onvif-recorder/boot-restore.sh` |
| `/etc/cron.d/onvif-recorder-backup` | Daily backup at 04:17 → `/data/onvif-recorder/backup.sh` |
| `/var/log/onvif-recorder-boot-restore.log` | Log of restore attempts (one block per boot). |

**Why outside the .deb?** Because `apt purge` removes everything dpkg
knows about. If the recovery scripts lived inside the package, purging
would wipe them along with the application, defeating the whole
point. Keeping them on `/data` and in `/etc/cron.d/` (neither managed
by dpkg) means a normal `apt remove` and `apt purge` leave the
recovery layer untouched, ready to re-install on next boot.

**Why gated on consent?** A user who explicitly purges the package
probably doesn't want a boot cron silently reinstalling it. The
`.autoupdate-consent` flag defaults to whatever `config.json` says
about `autoupdate_enabled`; it can also be flipped manually:

```bash
ssh root@<router> 'echo true  > /data/onvif-recorder/.autoupdate-consent'   # enable
ssh root@<router> 'echo false > /data/onvif-recorder/.autoupdate-consent'   # disable
```

### Uninstall — full purge

For a *complete* removal, including the recovery layer:

```bash
# 1. Remove the package (runs --rollback=all via prerm, then wipes
#    /etc/onvif-recorder, /var/lib/onvif-recorder, etc.):
apt-get purge -y onvif-recorder

# 2. Remove the persistent recovery layer (NOT managed by dpkg, so
#    apt purge does not touch these — must be removed by hand):
rm -rf /data/onvif-recorder
rm -f  /etc/cron.d/onvif-recorder-boot-restore
rm -f  /etc/cron.d/onvif-recorder-backup
rm -f  /var/log/onvif-recorder-boot-restore.log
rm -f  /var/log/onvif-recorder-backup.log
```

Skipping step 2 is intentional when the user only wants to clear apt
state but plans to reinstall — the next boot (or `apt install`) will
pick up the previous channel and config from the backup tarball. Only
do step 2 when the user really wants the device to forget the
application existed.

### APT suite promotion

Releases land in `early-access` first. Promote manually via the `promote.yml`
workflow (Actions → Promote apt suite → Run workflow):

- `early-access` → `rc` once the release has been exercised on a dev device
- `rc` → `stable` after wider testing

### Required release notes

Every release must have meaningful notes covering:
- **Highlights** — one paragraph per notable change, written for end users
- **New flags** — table of any new CLI flags with defaults and descriptions
- **Migration notes** — anything a user upgrading must do (or that happens automatically)
- **Dependencies** — any new third_party additions

### Steps (execute in order, in a single session)

```bash
# 1. Tag the release commit and push
git tag v<X.Y.Z>
git push origin v<X.Y.Z>

# 2. Build the ARM64 release binary (PGO + ThinLTO)
scripts/bz build --config=arm64_release //:onvif_recorder

# 3. Verify gh is authenticated BEFORE creating the release
gh auth status || { echo "Re-auth required"; exit 1; }

# 4. Create the release with notes + BOTH assets in one invocation
gh release create v<X.Y.Z> \
  --title "v<X.Y.Z>" \
  --notes "$(cat <<'NOTES'
## Highlights
...

## New flags
| Flag | Default | Description |
|------|---------|-------------|
| ... | ... | ... |

## Migration notes
...
NOTES
)" \
  ~/.cache/bazel/arm64_release/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder#onvif_recorder_arm64 \
  onvif-recorder.service

# 5. POST-RELEASE VERIFICATION (mandatory — do not skip)
gh release view v<X.Y.Z> --json name,body,assets --jq \
  '{name, body_chars: (.body | length), assets: [.assets[].name]}'
#   -> body_chars must be > 0
#   -> assets must include both "onvif_recorder_arm64" and "onvif-recorder.service"
```

If step 5 shows empty notes or missing assets, repair immediately with
`gh release edit v<X.Y.Z> --notes-file NOTES.md` and
`gh release upload v<X.Y.Z> <path>#<asset_name>` before doing anything else.

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
| `--model_dir` | `/usr/share/onvif-recorder/models` | Directory containing `nanodet_m.param` and `nanodet_m.bin`. Shipped inside the .deb; models are downloaded at runtime if not present. |
| `--state_dir` | `/var/lib/onvif-recorder` | Directory for runtime state (API-key cache, future per-host state). Defense-in-depth fallback: if the default is used and the legacy `/root/.config/onvif-recorder-api-key` exists, it is copied here on first run. |
| `--admin_port` | `7891` | localhost HTTP port serving the admin UI. Reached externally via the nginx-patched `https://<device>/onvif/admin/`. |
| `--channel_file` | `/etc/onvif-recorder/channel` | File containing the selected APT channel (`stable` / `rc` / `early-access`). Read by the admin UI and the `onvif-recorder-channel.service` oneshot. |
| `--detect` | `true` | Enable NanoDet-M as fallback when the camera provides no ONVIF bbox |
| `--detect_override` | `false` | Always run NanoDet-M, ignoring the ONVIF bbox entirely (implies `--detect`) |
| `--coalesce_window_sec` | `30` | Merge consecutive detections from the same camera into one event if the new detection starts within this many seconds of the previous one ending. Set to 0 to disable. |
| `--max_events_per_hour` | `10` | Maximum new detection events per camera per hour. Events beyond this limit are dropped. Set to 0 for unlimited. |
| `--coalesce_history` | `true` | On startup, scan the last `--coalesce_history_days` days of events and merge consecutive detections from the same third-party camera within `--coalesce_window_sec`. Only third-party (ONVIF) cameras are affected. |
| `--coalesce_history_days` | `30` | Number of days to look back when `--coalesce_history` is set. |
| `--disabled_object_types` | _(empty)_ | Comma-separated object types never recorded (`person`, `vehicle`, `animal`, `package`). |
| `--camera_disabled_object_types` | _(empty)_ | Per-camera `ip=types` pairs (types separated by `\|`). Replaces the global list for that camera; `ip=` re-enables all. |
| `--first_party_cameras` | _(empty)_ | Comma-separated camera IDs of first-party cameras to enable smart detection flags for in the cameras table. |
| `--first_party_camera_models` | _(empty)_ | Comma-separated model substrings to match first-party cameras (e.g. `G3 Instant,G4 Bullet`). Case-insensitive. Merged with `--first_party_cameras`. |
| `--poll_interval_sec` | `10` | Seconds between motion-event poll cycles for first-party cameras. |
| `--change_log` | _(empty)_ | Path for cameras-table change log (JSON Lines). Records old/new values for rollback. |
| `--rollback` | _(empty)_ | Undo cameras-table changes and exit. Values: `third_party`, `first_party`, `all`. |
| `--protect_url` | `http://localhost:7080` | Base URL for the local Protect API used to trigger automations on smart detection events. |
| `--protect_user_id` | _(auto-discovered)_ | X-UserId header for Protect API auth bypass. Auto-discovered from unifi-core DB on first run and cached to `<state_dir>/protect-user-id`. Pass explicitly to override. |
| `--msr_url` | `http://127.0.0.1:7700` | Base URL for the local UniFi Media Server Recording (MSR) gRPC service. Detection thumbnails are forwarded via `RecordingAPI.StoreSnapshots` so MSR persists them as native UBV files owned by `ms:unifi-streaming`, making third-party thumbnails indistinguishable from first-party. Set to empty string to disable. |
| `--trigger_automations` | `true` | Trigger Protect automations (push notifications, chimes, sirens) for detections onvif-recorder records. `false` keeps recording without notifying; Protect's own detections are unaffected. |
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

The `.deb` ships the NanoDet-M model files to
`/usr/share/onvif-recorder/models` and sets `--model_dir` to that path, so
no model-deploy step is needed on a package install.

Only required for the deprecated manual deploy below: copy the two files
yourself and point `--model_dir` at them:

```bash
ssh root@<router-ip> "mkdir -p /root/models"
scp nanodet_m.param nanodet_m.bin root@<router-ip>:/root/models/
# then in the systemd unit:
#   ExecStart=/root/onvif_recorder --detect --model_dir=/root/models
ssh root@<router-ip> "systemctl daemon-reload && systemctl restart onvif-recorder"
```

## Deploying to a Dream Router / NVR

Recommended: use the apt repo (one-line installer). For dev builds of an
unreleased commit:

```bash
# 1. Build the .deb (ARM64 release + PGO + LTO + dpkg-deb)
./build-in-docker.sh --deb

# 2. Copy + install
scp dist/onvif-recorder_*_arm64.deb root@<router-ip>:/tmp/
ssh root@<router-ip> "apt-get install -y /tmp/onvif-recorder_*_arm64.deb"
```

The .deb installs to `/usr/bin/onvif-recorder`, registers the systemd unit
under `/lib/systemd/system/`, and enables the service + the two daily timers
(channel sync + auto-update). Logs go to journald:
`journalctl -u onvif-recorder`.

### Legacy manual deploy (deprecated)

```bash
scripts/bz build --config=arm64_release //:onvif_recorder
scp ~/.cache/bazel/arm64_release/execroot/_main/bazel-out/k8-fastbuild/bin/onvif_recorder \
    root@<router-ip>:/root/onvif_recorder
scp onvif-recorder.service root@<router-ip>:/etc/systemd/system/
ssh root@<router-ip> "systemctl daemon-reload && systemctl restart onvif-recorder"
```
