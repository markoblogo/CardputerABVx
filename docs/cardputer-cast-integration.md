# Cardputer Cast integration with YTMamp

The Cardputer Music app can control playback on a desktop YTMamp instance over LAN.

## Why it exists

- Play remote music from YTMamp through Cardputer controls.
- Keep local SD playback as a fallback when the desktop endpoint is unavailable.
- Avoid hardcoding host/port by reading them from device settings.

## Configuration

Settings are stored in `/config/settings.json` and configured in **Network** app (Cast section).

- `castHost`: host/IP of the YTMamp machine (for example `192.168.4.1`)
- `castPort`: HTTP port of YTMamp local API (default `3000`)

Defaults are used when fields are missing:
- host: `192.168.4.1`
- port: `3000`

## Usage flow

1. Open **Network** and connect to Wi-Fi.
2. In **Network**, open `Cast Host` or `Cast Port`, edit values, and save.
3. Open **Music**.
4. Press `GO` (short) to switch between Local and CAST mode.
5. In CAST mode:
   - `SEL/ENT`: toggle
   - `RIGHT`: next
   - `LEFT`: prev
   - `UP`: play
   - `DOWN`: pause

## Supported API contract

Cardputer queries `GET /api/cast/status` and posts to `POST /api/cast/cmd`.
No token is required for MVP.

### GET `/api/cast/status`

Response shape (contract expectation):

```json
{
  "ok": true,
  "state": "playing|paused|stopped|error",
  "source": "ytmamp",
  "track": {
    "title": "string",
    "artist": "string",
    "album": "string",
    "track_id": "string",
    "duration_ms": 0,
    "position_ms": 0
  },
  "time": 1722500000000
}
```

If no active track is available:

```json
{
  "ok": false,
  "state": "stopped",
  "source": "ytmamp",
  "error": "no_active_track"
}
```

### POST `/api/cast/cmd`

Body:

```json
{"action":"toggle"}
```

Supported actions:
- `play`
- `pause`
- `toggle`
- `stop`
- `next`
- `prev`

On success response returns updated state in the same format as status endpoint.

## Diagnostics in UI

In CAST mode, **Music** screen shows:
- `Track ID` (`track.track_id`)
- `Err` (last error string)

These lines are useful to quickly check whether YTMamp endpoint responds correctly and which track it currently tracks.
