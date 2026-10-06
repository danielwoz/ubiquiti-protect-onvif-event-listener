# ONVIF Event Recorder

Bridges **third-party ONVIF cameras** into UniFi Protect. It listens to ONVIF
WS-PullPoint event streams and writes detection events (person, vehicle, animal,
package) to the UniFi Protect PostgreSQL database so they appear natively in the
Protect UI — including timeline clips, thumbnails, smart detection search, and
security alarms.

It can also add smart detection to **first-party UniFi cameras that lack native
AI** (e.g. G3 Instant). See [First-party camera support](#first-party-camera-support).

---

## Installation on Dream Router / Dream Machine

### Windows installer (no terminal required)

If you're on Windows and don't want to touch SSH directly, download the
latest `OnvifRecorderInstaller-v*.exe` from
[Releases](https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/releases)
and run it. The GUI:

1. Walks you through enabling SSH on your router
   (**UniFi OS → System → Advanced → SSH**).
2. Tests the connection with the root password you set.
3. Adds the apt repository + installs the package, streaming progress live.
4. Saves the credentials (password encrypted with Windows DPAPI) in
   `HKCU\Software\OnvifRecorderInstaller` so subsequent launches offer
   one-click upgrade, re-install, or uninstall.

The installer is unsigned, so Windows SmartScreen will prompt the first
time — click **More info → Run anyway**.

### One-line install (recommended for macOS/Linux users)

SSH to your UniFi device and run:

```bash
curl -fsSL https://danielwoz.github.io/ubiquiti-protect-onvif-event-listener/install.sh | sh
```

That's it. The installer:

1. Installs the signing key to `/usr/share/keyrings/onvif-recorder-archive-keyring.gpg`.
2. Detects your UniFi Protect release channel (Stable / Release Candidate /
   Early Access) and configures APT to pull matching builds.
3. `apt-get install onvif-recorder`, which auto-enables the systemd service
   plus two daily timers: channel sync and auto-updates.

Detections will appear in UniFi Protect within seconds of the first motion
event. Service logs land in journald (`journalctl -u onvif-recorder`).

**What happens on first start:**

1. The Protect user ID is **auto-discovered** from the unifi-core database
   and cached to `/var/lib/onvif-recorder/protect-user-id`.
2. The Protect UI is **live-patched** so third-party cameras appear in the
   alarm creation picker.
3. nginx is patched to expose the admin UI at `https://<device>/onvif/admin/`.
4. Third-party ONVIF cameras are loaded from the Protect database and smart
   detection flags are enabled for them.

No flags are required for the default setup.

### Admin UI

After install, manage the package without SSH at
`https://<device>/onvif/admin/`:

- Force an update check
- Switch release channel (stable / rc / early-access)
- Enable / disable auto-updates and change the schedule
- Uninstall the package

### Updates

Auto-updates are **on by default**. The `onvif-recorder-autoupdate.timer` runs
daily with up to 2h random jitter; `onvif-recorder-channel.timer` keeps the
APT suite in sync with your Protect channel. Both can be disabled via the
admin UI or `systemctl disable --now <timer>`.

### Uninstall

```bash
apt-get remove onvif-recorder          # stops service, rolls back DB changes
apt-get purge  onvif-recorder          # also removes /etc/onvif-recorder and state
```

### Legacy install (deprecated)

Prior releases shipped as a raw binary at `/root/onvif_recorder`. The `.deb`
postinst migrates an existing manual install automatically on first upgrade —
no action required.

---

## Configuration

For day-to-day changes — toggling NanoDet override, picking which first-party
cameras get smart-detect, raising the per-camera rate limit — use the admin
UI: open `https://<router-ip>/onvif/admin/` while logged in to UniFi OS.
Save & restart applies the change live.

The form covers detection, rate limiting, first-party cameras (tickbox list),
RTSP audio, and verbose logging.  Anything not set in the UI falls back to
the command-line flag.  See [`FLAGS.md`](FLAGS.md) for the full reference and
the more advanced flags that aren't in the UI (database paths, change log,
rollback, etc.).

### Example: enable verbose logging only

```
Admin UI -> Logging -> verbose -> true -> Save & restart
```

equivalent to:

```bash
echo 'ONVIF_RECORDER_FLAGS="--verbose"' \
    > /etc/default/onvif-recorder.local
systemctl restart onvif-recorder
```

### Example: route detections from a wildlife camera as `animal`

```
Admin UI -> Detection -> camera_object_types
  -> 192.168.1.108=animal -> Save & restart
```

## Notification Thumbnails
By default, object detection notifications will arrive with no thumbnail attached. In order to receive thumbnails as well, you'll need  to enable the global alarm manager.
Details on how to do this and the associated limitations are in [`THUMBNAILS.md`](THUMBNAILS.md)


## Troubleshooting

**Check the log for errors:**
```bash
journalctl -u onvif-recorder -f
```

**Enable verbose output** to see per-camera lifecycle events:
```bash
systemctl stop onvif-recorder
/usr/bin/onvif-recorder --verbose
```

**Camera not working?** Capture a raw diagnostic log and open a GitHub issue:
```bash
/usr/bin/onvif-recorder --verbose --raw_log=/tmp/onvif-raw.jsonl
# Let it run 60+ seconds, then Ctrl+C
# Attach /tmp/onvif-raw.jsonl to your issue
```

**Camera went offline and did not reconnect?** The recorder automatically retries.
After a run of failures it backs off, starting at 30 s and doubling up to 5 minutes,
and resets once a subscription succeeds. If a camera still does not come back,
restart the service:
```bash
systemctl restart onvif-recorder
```

**How many ONVIF subscribers should a camera show?** Two is expected: Protect keeps
its own event subscription per camera, and onvif-recorder holds the second. A third
can appear briefly while either side re-subscribes, until the old one times out
(about 60 s). Some cameras have too few subscription slots for both clients; they
reject one with errors such as `ter:BlockedThreadUpperLimit`, and Protect then
shows motion detection as misconfigured for that camera.

**Duplicate motion and "person" events?** From Protect 7.2, Protect subscribes to a
third-party camera's ONVIF motion itself and creates the basic motion events.
onvif-recorder never writes plain motion events, but by default it records motion
it cannot classify as a smart detection of `default_object_type` (person). On
Protect 7.2+ set `drop_unclassified_motion` to true (admin page, Detection) and
leave motion detection on in Protect. Cameras for which Protect shows motion
detection as misconfigured get no basic motion events with that setting.

---

## Building from source

### Prerequisites

- **Ubuntu 24.04** x86_64 build host (Ubuntu 22.04 also works)
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

All C libraries are built from source via Bazel. Only build tools are needed:

```bash
sudo apt-get install -y \
  build-essential clang cmake ninja-build perl \
  python3-cpplint
```

### 4. Build

```bash
# x86_64 native binary
scripts/bz build --config=x86 //:onvif_recorder

# ARM64 release binary (PGO + ThinLTO, for Dream Router / Dream Machine)
scripts/bz build --config=arm64_release //:onvif_recorder

# All tests
scripts/bz test --config=x86 //test:all
```

The ARM64 build downloads its own sysroot automatically — no manual cross-toolchain
setup is required.

### Runtime dependencies (ARM64)

The ARM64 binary is almost entirely statically linked:

```
libm.so.6  libc.so.6  ld-linux-aarch64.so.1  libgcc_s.so.1
```

### Runtime dependencies (x86_64)

```
libm.so.6  libc.so.6  ld-linux-x86-64.so.1  libgcc_s.so.1
libldap.so.2  liblber.so.2
```

---

## Project structure

```
main.cpp                      — Binary entry point
onvif_listener.hpp/.cpp       — WS-PullPoint ONVIF subscription library
detection_recorder.hpp/.cpp   — Detection -> PostgreSQL recorder
alarm_notifier.hpp/.cpp       — Protect automation trigger + history
motion_poller.hpp/.cpp        — First-party camera motion -> smart detect poller
camera_change_log.hpp/.cpp    — Cameras-table change log and rollback
protect_ui_patch.hpp/.cpp     — Live-patch Protect UI alarm picker
ubv_thumbnail.hpp/.cpp        — UBV container encode/decode
jpeg_crop.hpp/.cpp            — JPEG decode/crop/re-encode via libjpeg
object_detect.hpp/.cpp        — NanoDet-M object detection via NCNN
unifi_camera_config.hpp/.cpp  — Load camera credentials from Protect DB
```
