# Flags reference

The recorder reads its configuration from three places, in this order:

1. **Command-line flags** in `ExecStart` (or `ONVIF_RECORDER_FLAGS` in
   `/etc/default/onvif-recorder` / `/etc/default/onvif-recorder.local`).
2. **Config file overrides** at `/etc/onvif-recorder/config.json`.
   Non-empty values here override the command-line flag.  This is what
   the [admin UI](https://_router_/onvif/admin/) writes when you click
   **Save & restart**.
3. Built-in defaults if neither of the above is set.

`--rollback` is the only flag that is *never* overridden by the config
file (it is a one-shot CLI action; the config UI doesn't expose it).

To set a flag from the shell instead of the admin UI, the recommended
path on a packaged install is to edit `/etc/default/onvif-recorder.local`
(it is not a conffile so dpkg can never clobber it across upgrades):

```bash
echo 'ONVIF_RECORDER_FLAGS="--verbose --detect_override"' \
    > /etc/default/onvif-recorder.local
systemctl restart onvif-recorder
```

## Detection object types

The recorder maps ONVIF camera events to four UniFi Protect smart-detection types:

| Type | How it is produced |
|------|--------------------|
| `person` | FieldDetector "Human", HumanShapeDetect, Reolink PeopleDetect, or any generic motion event when `--default_object_type=person` (the factory default) |
| `vehicle` | FieldDetector "Vehicle", VehicleDetect, Reolink VehicleDetect, or generic motion when `--default_object_type=vehicle` |
| `animal` | Generic motion when `--default_object_type=animal`, or any camera whose events are overridden via `--camera_object_types` |
| `package` | Generic motion when `--default_object_type=package`, or any camera whose events are overridden via `--camera_object_types` |

**Generic motion events** (CellMotionDetector, VideoSource/MotionAlarm) carry no
object class. By default the recorder uses `--default_object_type` for these.
When NanoDet-M is enabled (the default), the detected COCO class overrides the
default type — so a snapshot containing a car is stored as `vehicle` even though
the camera reported only "motion". Per-camera `--camera_object_types` overrides
always take priority over NanoDet-M.

AI-specific events (Human/Vehicle detections from FieldDetector, HumanShapeDetect,
etc.) are never affected by `--default_object_type` or NanoDet-M class inference.

**Per-camera overrides** (`--camera_object_types`) replace the detection type for
every event from the named camera, including AI events. This is useful for
wildlife cameras or entrance monitors where you always want a specific type
regardless of what the camera reports.

| Flag | Default | Description |
|------|---------|-------------|
| `--default_object_type` | `person` | Object type written for generic motion events that carry no object class. Valid values: `person`, `vehicle`, `animal`, `package`. |
| `--camera_object_types` | _(disabled)_ | Comma-separated `ip=type` overrides applied to all events from each named camera, e.g. `192.168.1.108=animal,192.168.1.109=package`. |
| `--drop_unclassified_motion` | `false` | Drop generic motion events (CellMotionDetector / MotionAlarm) that carry no ONVIF object class and that NanoDet-M cannot classify, instead of recording them as `--default_object_type`. Real camera AI events (Person / Vehicle / Pet) and `--camera_object_types` overrides are unaffected. Cuts false-positive clutter from AI cameras (e.g. Reolink) that also emit noisy basic-motion events. |

**Example — wildlife camera on `.108` and package camera on `.109`:**
```bash
ExecStart=/usr/bin/onvif-recorder \
  --camera_object_types=192.168.1.108=animal,192.168.1.109=package
```

---

## Security alarms

The recorder integrates with the UniFi Protect automation system so that ONVIF
camera detections can trigger automations (sound, push notification, webhook,
unlock door, light, PTZ preset) configured in **Protect → Alarms**.

**How it works:**

1. At startup the recorder live-patches the Protect UI so third-party cameras
   appear in the alarm creation scope picker.
2. The recorder refreshes the list of configured automations from the Protect API
   every five minutes.
3. When a detection event is recorded, the recorder triggers matching automations
   via the local Protect API (port 7080), records history in `automationsHistory`
   so hits appear in the Protect UI, and respects the "ignore repeated" cooldown
   setting on each automation.

**Setup:**

1. In Protect, create a new alarm under **Protect → Alarms → New alarm**.
2. Set the scope to include your ONVIF cameras and choose the trigger types you
   want (person, vehicle, animal, and/or package).
3. Choose one or more actions: sound, push notification, webhook, etc.
4. Your ONVIF cameras will fire the alarm on matching detections.

| Flag | Default | Description |
|------|---------|-------------|
| `--protect_url` | `http://localhost:7080` | Base URL for the local Protect API. Only change this if running off-device. |
| `--protect_user_id` | _(auto-discovered)_ | Protect user ID for API auth. Auto-discovered from the unifi-core DB and cached to `/var/lib/onvif-recorder/protect-user-id`. Pass explicitly to override. |
| `--patch_alarm_picker` | `true` | Live-patch the Protect UI so third-party cameras appear in the alarm creation picker. |

---

## NanoDet-M object detection

NanoDet-M is enabled by default for thumbnail subject cropping and object
classification. Model files are **downloaded automatically** on first startup if
not present in `--model_dir`.

> **Performance (Dream Router — 4 x Cortex-A55 cores):**
> NanoDet-M takes ~158 ms per inference on one core (ARM64 NEON build).
> At 1 detection/minute this is 0.26% of 1 core = **0.065% of total CPU**.
> Even at 5 detections/minute across multiple cameras you are well under 0.5%
> of total device CPU.

| Flag | Default | Description |
|------|---------|-------------|
| `--model_dir` | `/usr/share/onvif-recorder/models` | Directory for NanoDet-M model files. Shipped inside the .deb. |
| `--detect` | `true` | Run NanoDet-M as fallback when the camera provides no ONVIF bounding box. |
| `--detect_override` | `false` | Always run NanoDet-M, ignoring any ONVIF bounding box. Implies `--detect`. |

**Thumbnail crop priority:**
1. `--detect_override` set: always use NanoDet-M
2. Camera provides an ONVIF bounding box: crop to that box
3. `--detect` set and no ONVIF box: run NanoDet-M, fall back to full image
4. `--nodetect`: full uncropped image when no ONVIF box

To disable NanoDet-M entirely, pass `--nodetect`.

---

## First-party camera support

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

**Optional: enable the Protect UI smart detection panel** for specific cameras
by ticking them in the admin UI's *First-party cameras* card, or passing their
database IDs via `--first_party_cameras`.  This updates
`featureFlags.smartDetectTypes` and `smartDetectZones` in the cameras table so
the Protect UI shows smart detections for those cameras.  Without this, the
detection events are still recorded and alarms still fire — you just won't see
them in the smart detection timeline filter.

To find your camera IDs from the shell:

```sql
psql -h /run/postgresql -p 5433 -U postgres unifi-protect -c "
SELECT id, name, type
FROM cameras
WHERE \"isThirdPartyCamera\" = false AND \"isAdopted\" = true
ORDER BY name;
"
```

The `type` column holds the model string (e.g. `UVC G3 Instant`,
`UVC G4 Doorbell Pro`).  The cameras table has no `model` column.

| Flag | Default | Description |
|------|---------|-------------|
| `--first_party_cameras` | _(disabled)_ | Comma-separated camera IDs to enable smart detection flags for. Only needed for the Protect smart detection UI. |
| `--first_party_camera_models` | _(disabled)_ | Comma-separated model substrings to match (e.g. `G3 Instant,G4 Bullet`).  Merged with `--first_party_cameras`. |
| `--poll_interval_sec` | `10` | Seconds between motion-event poll cycles. |

**Example — enable G3 Instant smart detection with UI support:**
```bash
ExecStart=/usr/bin/onvif-recorder --detect_override \
  --first_party_cameras=6713fe0a01583d03e400051c,6713fa9e023c3d03e4000451
```

---

## Change log and rollback

The recorder can log every cameras-table modification it makes to a JSON Lines
file and undo those changes on demand.

| Flag | Default | Description |
|------|---------|-------------|
| `--change_log` | _(disabled)_ | Path for the cameras-table change log (JSON Lines). |
| `--rollback` | _(disabled)_ | Undo cameras-table changes and **exit**. Values: `third_party`, `first_party`, `all`.  Always taken from the command line; never read from `config.json`. |

When `--rollback` is set, the recorder reads the change log (if it exists),
applies the original values back to the database, and exits. If no change log
file exists and the scope includes third-party cameras, the recorder performs a
best-effort reset.

**Example — enable change logging:**
```bash
ExecStart=/usr/bin/onvif-recorder \
  --change_log=/var/lib/onvif-recorder/cam_changes.jsonl
```

**Example — rollback all changes and exit:**
```bash
/usr/bin/onvif-recorder --rollback=all \
  --change_log=/var/lib/onvif-recorder/cam_changes.jsonl
```

---

## Other flags

| Flag | Default | Description |
|------|---------|-------------|
| `--db_conn` | `host=/run/postgresql port=5433 dbname=unifi-protect user=postgres` | libpq connection string for the Protect database. |
| `--db_host` | _(empty = Unix socket)_ | Override PostgreSQL host for camera config loading. |
| `--ubv_dir` | _(auto-detected)_ | Directory for per-camera UBV thumbnail files. Auto-detected on Dream Routers (`/srv/unifi-protect/video`). |
| `--rtsp_audio` | _(disabled)_ | Set RTSP audio in the Protect database: `enable` or `disable`. |
| `--pre_buffer_sec` | `2` | Seconds before the first detection to mark as clip start. |
| `--post_buffer_sec` | `2` | Seconds after the last detection to mark as clip end. |
| `--coalesce_window_sec` | `30` | Merge consecutive detections within this window into one event. `0` to disable. |
| `--max_events_per_hour` | `10` | Maximum new events per camera per hour. `0` for unlimited. |
| `--coalesce_history` | `true` | On startup, merge consecutive historical detections within `--coalesce_window_sec`. |
| `--coalesce_history_days` | `30` | Days to look back for history coalescing. |
| `--verbose` | `false` | Enable INFO-level logging (lifecycle, events, renewals). |
| `--event_log` | _(disabled)_ | Path for parsed-event JSON Lines log. |
| `--raw_log` | _(disabled)_ | Path for raw SOAP exchange JSON Lines log (large). |
| `--admin_port` | `7891` | Port for the local admin HTTP server (loopback only). |
| `--log_port` | `7890` | Port for the local log-viewer HTTP server (loopback only). |
| `--state_dir` | `/var/lib/onvif-recorder` | Directory for persistent runtime state (cached user IDs, etc). |
| `--channel_file` | `/etc/onvif-recorder/channel` | Path to the apt channel file (`stable` / `rc` / `early-access`). |
| `--msr_url` | `http://127.0.0.1:7700` | URL of the local MSR gRPC service that stores thumbnails as native UBV.  Empty disables MSR forwarding. |
