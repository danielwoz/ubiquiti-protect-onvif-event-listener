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
3. Selects the release channel matching your Protect channel. Keep it and
   onvif-recorder follows Protect when you change Protect's channel; pick
   another and onvif-recorder stays on that one until you change it on the
   admin page.
4. Adds the apt repository + installs the package, streaming progress live.
5. Saves the credentials (password encrypted with Windows DPAPI) in
   `HKCU\Software\OnvifRecorderInstaller` so subsequent launches offer
   one-click upgrade or uninstall, and re-install after confirming the
   channel.

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
   alarm creation picker, and Protect's own ONVIF client is patched to send
   the WS-Addressing headers some cameras (Reolink) require. Protect loads
   these changes only when it starts, so if it was already running it is
   restarted about five minutes later. The same happens after a Protect or
   UniFi OS update.
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

## Sharing a camera's events with Protect

From Protect 7.1, Protect subscribes to each third-party camera's ONVIF events itself,
alongside onvif-recorder's own subscription. Most cameras accept both. Some, such as
certain PTZ cameras, accept only one event subscriber, so the two take turns: Protect
shows the camera's motion detection as misconfigured and logs `ter:BlockedThreadUpperLimit`
errors, and onvif-recorder misses events while Protect holds the camera's only slot.

Event sharing is meant for those cameras. For a shared camera, onvif-recorder keeps the
only event subscription to the camera and passes the events it receives on to Protect, so
the camera should see a single subscriber. Sharing is off by default and is turned on per
camera.

**Turning it on.** On the admin page (`https://<device>/onvif/admin/`), tick
**Share with Protect** for the camera on the **Camera health** card and click
**Save sharing changes**. Without the admin page, set `protect_event_proxy_cameras` to the
cameras' MAC addresses, comma-separated (case and `:` or `-` separators don't matter); see
[`FLAGS.md`](FLAGS.md).

**What happens:**

- Protect reads the change only when it starts, so onvif-recorder restarts Protect about
  five minutes after the first camera is turned on, and again about five minutes after the
  last one is turned off. Turning other cameras on or off in between restarts only
  onvif-recorder. With `--auto_heal_protect=false`, restart Protect yourself.
- When Protect subscribes to a shared camera's events, it asks onvif-recorder first. If
  onvif-recorder is not running, declines, or does not answer within a few seconds, Protect
  subscribes to the camera itself, as it does without sharing. Time during which Protect
  itself is stalled (by a long garbage collection, for example) does not count towards
  those seconds. One camera that gets no answer does not affect the others; once two get
  none, Protect stops waiting for onvif-recorder for half a second, so an onvif-recorder
  that has stopped answering does not hold up Protect's polling of every camera in turn.
- Only the event subscription goes through onvif-recorder. Protect's other requests to the
  camera (video, snapshots, PTZ, its check of the camera's event support) still go to the
  camera.
- When onvif-recorder restarts for a settings change or a package update, it keeps its
  subscription to the camera, so in most cases Protect should see no gap in events.
- The Camera health card shows under each ticked camera where Protect gets its events
  from right now: `via onvif-recorder` while Protect pulls them from onvif-recorder, or
  `direct (…)` with the reason while Protect is subscribed to the camera itself, such as
  `direct (Protect restart pending)` or `direct (handed back: silent)`. Log lines about
  sharing start with `[event_proxy]` (`journalctl -u onvif-recorder`); the warnings among
  them, such as a hand-back or Protect moving to onvif-recorder, are logged even without
  `--verbose`.

**Limitations:**

- Needs Protect 7.1 or later and `patch_alarm_picker` (on by default), because sharing works
  by patching Protect. If a Protect version doesn't match what onvif-recorder expects,
  sharing is not applied, the admin page says this Protect version can't share events, and
  Protect keeps subscribing to the camera directly.
- onvif-recorder must be able to subscribe to the camera itself, since its subscription is
  where the shared events come from. Cameras that are excluded on the Camera health card or
  paired with an AI Port can't be shared.
- If onvif-recorder can't get events from the camera for a while (from a few minutes up to
  about half an hour, depending on how it fails), it hands the camera back to Protect, which
  then subscribes to the camera directly again. The status shows `direct (handed back: …)`
  and the log has a warning. onvif-recorder should take the camera back once its own
  subscription has worked again for about five minutes; a restart of onvif-recorder does
  not reset this.
- Turning sharing off for one camera while others stay shared puts that camera back to how
  it was before, including the subscriber conflict on a single-subscriber camera.
- onvif-recorder serves Protect on port 7892 of the loopback address only. If another
  program already uses that port, sharing stays off and the admin page says so.

## Keeping your own Protect patches

onvif-recorder patches Protect's `service.js` (in `/usr/share/unifi-protect/app`). Whenever
it changes its patches, it rebuilds the file from Protect's original copy
(`service.js.bak`), so edits you make to `service.js` by hand are lost. To keep your own
changes, list them in `/data/onvif-recorder/local-patches.json` (set `--local_patches` to use
another path). onvif-recorder applies them after its own patches each time it patches
Protect, so they come back by themselves after a Protect or UniFi OS update.

```json
[
  {
    "name": "my change",
    "file": "service.js",
    "original": "exact text copied from service.js",
    "replacement": "the text to put in its place"
  }
]
```

- `file` must be `service.js`. `name` is optional; it is shown in the log and on the admin
  page.
- `original` is plain text, not a pattern, and must occur exactly once in `service.js`.
  Include enough of the surrounding code to make it unique. In JSON, write `"` as `\"` and
  `\` as `\\`.
- `replacement` must not be empty and must not contain `original`.
- The file is read when onvif-recorder starts, so restart it after editing
  (`systemctl restart onvif-recorder`). Removing an entry undoes it.

**What happens:**

- Protect reads `service.js` only when it starts, so onvif-recorder restarts Protect about
  five minutes after it changes the file. That restart counts towards the four automatic
  Protect restarts allowed per day. With `--auto_heal_protect=false`, restart Protect
  yourself.
- The **Status** card on the admin page shows how many entries are applied and names any that
  are not. Log lines start with `[ui_patch] local patch`. An entry that doesn't apply is
  logged as a warning.
- If the file can't be read or isn't valid (bad JSON, a missing or unknown key, a `file`
  other than `service.js`), none of its entries is applied, and the error is logged and shown
  on the Status card. onvif-recorder's own patches still apply.
- The file is on `/data`, so UniFi OS updates and `apt purge` leave it in place.

**Caveats:**

- Entries are plain find/replace on Protect's minified code. A Protect update can change that
  code, and the entry then stops applying until you update it. Check the Status card after a
  Protect update.
- Entries are applied after onvif-recorder's own patches. An entry whose `original` overlaps
  code that one of those patches changes does not apply while that patch is on.
- `patch_alarm_picker=false` turns off all patching, these entries included.
- Removing an entry undoes it only while `service.js.bak` is still Protect's original, which
  onvif-recorder checks against dpkg's checksums. If you edited `service.js.bak` by hand
  before, undo those edits so the two match again:
  ```bash
  md5sum /usr/share/unifi-protect/app/service.js.bak
  grep 'app/service.js$' /var/lib/dpkg/info/unifi-protect.md5sums
  ```
  Otherwise the next Protect update replaces both files and puts this right.

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

**`ter:BlockedThreadUpperLimit` errors, or motion detection shown as misconfigured, on a
camera that allows only one event subscriber?** Protect and onvif-recorder are competing
for the camera's only subscription slot, and onvif-recorder may miss events too. Tick
**Share with Protect** for that camera on the Camera health card (admin page), so
onvif-recorder holds the only subscription and passes the events on to Protect; see
[Sharing a camera's events with Protect](#sharing-a-cameras-events-with-protect). A Reolink
camera can show the same status for a different reason; see below.

**Third-party camera records nothing after Protect restarts?** Protect decides whether to
record each camera when it starts. A third-party camera in Events Only (or Adaptive) mode
whose motion detection shows as misconfigured at that moment, and for which Protect has not
loaded onvif-recorder's smart detection setting, is not recorded, and Protect does not check
again when motion detection becomes available. Its motion events are then dropped until
Protect restarts. onvif-recorder checks for this every minute once Protect has run for five
minutes: when the same camera is still not recording a minute later while other cameras
record, it re-applies the camera's smart detection setting and restarts Protect
(`[healer] recording-stuck` in the log; not with
`--auto_heal_protect=false`, and within the limit of 4 Protect restarts a day). It does this
once per camera: if the camera is still not recording after that restart, onvif-recorder
reports it on the admin page's Recent errors card and waits until Protect records the
camera again before it would restart Protect for it another time. To fix it by hand, once Protect shows the camera's motion detection as
available, switch the camera's recording mode to Always and back, or restart Protect.

**Duplicate motion and "person" events?** From Protect 7.1, Protect subscribes to a
third-party camera's ONVIF motion itself and creates the basic motion events.
onvif-recorder never writes plain motion events, but by default it records motion
it cannot classify as a smart detection of `default_object_type` (person). On
Protect 7.1+ set `drop_unclassified_motion` to true, or `default_object_type` to
`motion`, which does the same (admin page, Detection), and leave motion detection
on in Protect. Cameras for which Protect shows motion detection as misconfigured
get no basic motion events with that setting, and neither do any third-party
cameras on Protect older than 7.1.

**No native motion from a Reolink camera, or Protect shows its motion detection as
misconfigured?** From Protect 7.1, Protect subscribes to the camera's ONVIF motion
events itself, but its requests lack two WS-Addressing headers that Reolink requires,
so the camera rejects them. On 7.2 and later Protect then shows motion detection as
misconfigured; on 7.1 it shows it as available but records no motion events.
onvif-recorder patches Protect to send the headers (`patch_onvif_addressing`, on by
default) and restarts Protect about five minutes later so the change loads (with
`--auto_heal_protect=false`, restart Protect yourself). A camera with too few
subscription slots can show the same status; see above.

**Camera missing from the AI Port pairing list?** Protect only offers cameras without
licence-plate or face detection, and onvif-recorder marks the cameras it manages as
licence-plate capable so their detections panel shows. Untick a third-party camera on
the Camera Health card (admin page): the recorder removes that marker and, if needed,
restarts Protect about five minutes later so it applies (with
`--auto_heal_protect=false`, restart Protect yourself). Cameras already paired with an
AI Port are left alone.

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
