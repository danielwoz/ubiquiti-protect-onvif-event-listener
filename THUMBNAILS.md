# Push Notification Thumbnails

This document explains how push notification thumbnails work for third-party
(ONVIF) cameras on a UNVR running UniFi Protect 7.x, and what you must
configure to enable them.

---

## Prerequisites

### Enable Global Alarm Manager in Protect

Protect has two alarm manager modes:

| Mode | Description |
|------|-------------|
| **Local** (default) | Automations are managed by the Protect process in isolation. onvif-recorder evaluates them itself and asks Protect's automation manager to notify; when those routes are absent it falls back to `/api/automations/<id>/run`, which carries no event and so no thumbnail. |
| **Global** | Alarms are kept by UniFi OS (uos-agent). onvif-recorder hands each detection to UniFi OS (`POST 127.0.0.1:11010/api/v1/alarms/events`) with the Protect event id and camera id, the same request Protect sends for its own cameras. |

**Steps to enable:**

1. Open the UniFi Protect web UI.
2. Go to **Settings -> Advanced -> Alarm Manager Mode**.
3. Switch from **Local** to **Global**.
4. Protect will migrate your existing automations to the global store automatically.

> **Why this matters:** Protect's notify action builds the push from the
> event: it looks the camera up by its DB id, reads the event row and then its
> thumbnail.  In Global mode onvif-recorder sends UniFi OS the event with
> `eventId` (the `events.id` it inserted) and `sourceEvent.camera` (the
> camera's `cameras.id`), or only `eventId` when the camera id is not known,
> after the event row and thumbnail are written.  Whether a Global-mode push
> for a third-party camera carries the thumbnail has not yet been confirmed on
> a console; if it does not, `/data/unifi-core/logs/uos-agent.log` and Protect's
> logs are the places to look.

> **Alternative (not recommended):** The equivalent manual DB change is:
> ```sql
> UPDATE nvrs
>    SET "featureFlags" = replace(
>          "featureFlags"::text,
>          '"useExternalAlarmManager":false',
>          '"useExternalAlarmManager":true')::json;
> ```
> Use this only as a recovery step if the UI migration fails — a Protect update
> may overwrite it.

---

## Notification Format

The notification title and body are formatted by Protect's own templates,
not by onvif-recorder.  We do not send `title` or `body` fields in the
payload; Protect determines them from the automation type and the resolved
camera/event data.

Confirmed output (from `service.log`, Protect 7.1.83):

```
Sending push with thumbnail for type "smart-detect":
{
  "title": "Smart Detection Recorded",
  "body":  "UNVR's Gate has recorded a person."
}
```

The camera name ("Gate") is resolved via the camera's DB id onvif-recorder
passes (`sourceEvent.cameraId` on the Local notify route, `sourceEvent.camera`
in the Global events it sends UniFi OS).  The NVR name ("UNVR") comes from the NVR model.
The object type ("a person") comes from the event key.

---

## Timeline Deep Link

**Deep linking from a notificaiton to the event in the timeline is not working.**
The push notification deep link is entirely constructed by Protect's sendNotificationAction — we have no control over it. For native camera detections, Protect includes routing data in the push payload's data object (something like {"eventId": "...", "cameraId": "..."}) that the mobile app parses to navigate directly to the clip.

For the Local automation-manager notify path, Protect logs only:

"title": "Smart Detection Recorded",
"body": "UNVR's Front Yard has recorded a person."
The absence of navigation data in the FCM/APNs data object means the app receives the notification but has no event coordinate to route to, so it falls back to the "Find anything" search page. This is a limitation of how Protect's external alarm manager push action is implemented — it doesn't include the same eventId/cameraId routing fields that the native camera push path does.

On that path there is nothing in our payload we can change to fix this. We already pass eventId and sourceEvent.cameraId — Protect receives them, resolves the event, attaches the thumbnail, but still doesn't include routing data in the outbound push. That's a Protect-side gap in the external alarm manager implementation.

In Global mode the detection reaches Protect's notify action through UniFi OS in the same form as Protect's own cameras' events (`eventId` and `sourceEvent.camera`). Whether that adds the deep link for third-party cameras has not been checked yet.

The tap-to-clip behaviour is something only Ubiquiti can fix by updating sendNotificationAction for the external path to include eventId in the push data object — the same way it does for native cameras.