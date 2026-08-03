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
- `castDebug`: enable cast endpoint logging on Cardputer (`true|false`, default `false`)

Defaults are used when fields are missing:
- host: `192.168.4.1`
- port: `3000`

The same values are also shown in the `Network` app in **Cast** section and persisted to:
`/config/settings.json`.

Current format:

```json
{
  "castHost": "192.168.4.1",
  "castPort": 3000,
  "castDebug": false
}
```

## Network contract (host/port + status/cmd endpoints)

Cardputer uses two API paths with fallback:

- `GET /api/cast/status` then fallback `GET /cast/status`
- `POST /api/cast/cmd` then fallback `GET /cast/{action}`

### Compatibility shim contract

Для простоты интеграции с чужими версиями YTMamp Cardputer ожидает одинаковый минимальный JSON-ответ на статус-контракт:

- `state` — строка статуса: `playing|paused|stopped|error`
- `track` — объект с полями `title`, `artist`, `album`, `track_id`, `duration_ms`, `position_ms`
- `time` — текущее время/позиция в миллисекундах
- `error` — человекочитаемый текст ошибки (опционально)

Сервер может возвращать расширенный JSON или иные форматы (например `playing`, `paused`, `status`, `track_id`, `duration`, `position`, `timestamp`) — Cardputer-скрипт их нормализует в единый контракт.

Минимальная совместимая форма, если `track` отсутствует:

```json
{
  "ok": false,
  "state": "stopped",
  "error": "no_active_track",
  "time": 0
}
```

Поддерживается и прямой статус `GET /status` как fallback-совместимость для простых локальных инстансов.

Actions supported for command endpoint:
- `toggle`, `play`, `pause`, `next`, `prev` (and optional `stop`).

Requests are JSON based:

- `GET /api/cast/status` response is JSON status object.
- `POST /api/cast/cmd` body:

```json
{"action":"toggle"}
```

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

Cardputer queries the status endpoint and sends commands through the cmd endpoint.
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

## Retry/timeout policy

- request timeout: `1500 ms`
- attempts: `2` per endpoint
- short inter-attempt delay: `90 ms`
- fallback endpoints as above when API path differs
- client retry budget in app is also applied by UI polling cooldown:
  - normal poll cadence: `~1500 ms`
  - soft backoff after failures: `120/700/1000 ms` with longer fallbacks on repeated errors

## Diagnostics in UI

In CAST mode, **Music** screen shows:
- `Track ID` (`track.track_id`)
- `Err` (last error string)
- `Net` line with last status:
  - `on/off`
  - attempt count
  - last RTT in ms

These lines are useful to quickly check whether YTMamp endpoint responds correctly and which track it currently tracks.

## Short device validation checklist

Use this sequence on Cardputer to validate that Cast integration works end-to-end:

1. Open **Network**, connect to Wi-Fi, then set:
   - `Cast Host` to the PC LAN IP (for example `192.168.4.1`)
   - `Cast Port` to YTMamp API port (`3000`)
2. Open **Music**, press `GO` once to enter CAST mode.
3. With YTMamp playing a playlist on PC, verify:
   - `Mode: CAST (...)` appears.
   - `Track`, `Artist`, `Album`, `Track ID`, and `Err` lines update every 1.5s.
4. Press:
   - `SEL/ENT` → state toggles pause/play on PC.
   - `RIGHT` → next track.
   - `LEFT` → previous track.
   - `UP` → play.
   - `DOWN` → pause.
   - UI should show temporary status such as `cast ok` and track metadata updates.
5. Edge-case checks:
   - **no active track**: stop music on PC or clear queue and open CAST status. Expected:
     - Cardputer still in CAST mode.
     - `Err` contains `no_active_track`.
     - Track line is empty/idle (`no track`/blank title).
   - **desktop offline**: disconnect YTMamp network or stop service.
     - Cardputer shows `cast offline` and mode still responds.
     - Playback controls still work on SD player via local mode when toggling `GO` back to Local.
     - No crash/restart observed during repeated `toggle/next/prev`.
- In **Network**, press `D` in normal mode to toggle `Cast Debug`.
  - when ON, settings will persist as `castDebug` in `/config/settings.json`.
- In **Network**, line `Cast Debug` shows current mode.
- In **Music** CAST mode, extra diagnostic line is shown:
  - `DBG: <code> <path>`
  - `<code>` is HTTP status of last request.
  - `<path>` is the path actually used (`/api/cast/status`, `/cast/status`, `/cast/{action}`).
- Debug mode also writes short path/code notes to SD logs.
