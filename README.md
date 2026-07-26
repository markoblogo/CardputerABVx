# M5Stack Cardputer ADV Unified Shell

Integrated PlatformIO firmware shell for M5Stack Cardputer ADV. It uses one menu, one app interface, centralized input, centralized power handling, centralized SD paths, and terminal-style UI.

## System Map

```mermaid
flowchart LR
    Device["Cardputer hardware<br/>keyboard · screen · speaker · mic · Wi-Fi · SD"]
    Core["Core managers<br/>Input · Power · TerminalUI · Storage · Settings · App · Network"]
    Apps["App shell and feature apps"]
    SD["SD card content and config"]
    Services["Local/device-facing services<br/>HTTP file manager · AI endpoint · NTP · Wi-Fi"]
    User["On-device user flows"]

    Device --> Core
    Core --> Apps
    SD --> Core
    SD --> Apps
    Services --> Core
    Apps --> User
    User --> Device
```

Keep this map updated when hardware dependencies, core manager ownership, SD layout, or external service boundaries change.

## Build

```sh
platformio run -e m5stack-cardputer-adv
```

Environment:

- Board: `m5stack-stamps3`
- Framework: Arduino
- Main libraries:
  - `m5stack/M5Cardputer`
  - `bblanchon/ArduinoJson`
  - `earlephilhower/ESP8266Audio@1.9.7`

## Architecture

Apps implement:

- `begin(AppContext&)`
- `update()`
- `draw()`
- `onInput(InputEvent event)`
- `getTitle()`
- `getHelpLine()`

Raw keyboard/button input is only allowed in `src/core/InputManager.cpp`. Apps consume semantic `InputEvent` values only.

Shared managers:

- `InputManager`
- `PowerManager`
- `TerminalUI`
- `StorageManager`
- `SettingsManager`
- `AppManager`
- `NetworkManager`

Reusable components:

- `FileBrowser`
- `TextEditor`

## Controls

- BtnGO short press: primary/select/play/pause/start/pause by app context.
- BtnGO long press: Back/Menu.
- Enter: confirm/open/send; newline in editors unless overridden.
- Arrows: navigation, scroll, values.
- Text keys: only text fields/editors.
- Backspace: delete in text fields.
- First key/button while screen is off emits only `Wake`.

Every screen has a one-line footer.

## Feature Flags

Configured in `include/Features.h`.

| Flag | Default |
|---|---:|
| `FEATURE_MUSIC` | 1 |
| `FEATURE_RECORDER` | 1 |
| `FEATURE_NOTES` | 1 |
| `FEATURE_READER` | 1 |
| `FEATURE_CLOCK` | 1 |
| `FEATURE_NETWORK` | 1 |
| `FEATURE_WEB_FILE_MANAGER` | 1 |
| `FEATURE_RANDOMIZER` | 1 |
| `FEATURE_BROWSER` | 1 |
| `FEATURE_AI_TEXT` | 1 |
| `FEATURE_AI_VOICE` | 0 |
| `FEATURE_PAYMENTS_INFO` | 1 |
| `FEATURE_INPUT_DIAGNOSTICS` | 1 |

## Feature Status

| Module | Status | Notes |
|---|---|---|
| Main menu/AppManager | Implemented | Integrated app shell and return-to-menu behavior. |
| InputManager | Implemented | Semantic events, debounce, repeat, long press, wake suppression. |
| PowerManager | Implemented | Central screen-off/backlight handling; no deep sleep yet. |
| TerminalUI | Implemented | Shared terminal-style black/green/yellow UI. |
| StorageManager | Implemented | SD init, folder creation, text read/write, logs, listing. |
| SettingsManager | Implemented | JSON settings load/save. |
| FileBrowser | Implemented | Reusable SD file listing and selection. |
| TextEditor | Implemented | Reusable text input, newline, cursor, delete. |
| MusicApp | Partial | Real MP3 decode via ESP8266Audio into M5 Speaker, SD file list, pause/resume, next, shuffle, volume. |
| RecorderApp | Partial | Real PCM WAV capture through M5 Mic, RIFF header patching, pause/resume/save flow. |
| NotesApp | Implemented | List/create/open/edit/save/autosave draft. |
| ReaderApp | Implemented | TXT listing, normal/speed modes, scrolling/WPM controls. |
| ClockApp | Partial | Clock, NTP sync, stopwatch, timer, completion tone, screen-off counting. |
| NetworkApp | Partial | Scan, open/locked markers, RSSI, password entry, saved Wi-Fi, reconnect, status. |
| WebFileManagerApp | Partial | HTTP root, JSON list, download, upload, mkdir, delete, path checks. |
| RandomizerApp | Implemented | Yes/No/Maybe, number range, SD text-file list picker. |
| BrowserApp | Partial | URL/search fetch, text extraction, link extraction/selection, back history, save page. |
| AIApp | Partial | Config-driven HTTPS POST, OpenAI-compatible payload, JSON response parsing, logs to `/ai`. |
| AI voice | Disabled | `FEATURE_AI_VOICE=0`. |
| PaymentsApp | Implemented | Informational only; no card storage/emulation. |
| InputDiagnosticsApp | Implemented | Shows last semantic event and wake suppression. |
| SystemInfoApp | Implemented | SD, heap, Wi-Fi, IP, screen, build/features. |

## SD Card Layout

Copy `sdcard_template/` contents to the SD root. The firmware also creates missing folders:

- `/music`
- `/recordings`
- `/notes`
- `/books`
- `/browser`
- `/browser/bookmarks`
- `/browser/saved_pages`
- `/ai`
- `/config`
- `/logs`
- `/tmp`

Config files:

- `/config/settings.json`
- `/config/wifi.json`
- `/config/ai.json`
- `/config/bookmarks.json`
- `/config/app_state.json`
- `/config/randomizer_lists.txt`

## Known Limitations

- MP3 playback uses M5 Speaker buffered PCM output; Cardputer ADV audio quality/volume needs real-device tuning.
- Recorder uses M5 Mic chunk capture; sample stability and pauses need real-device validation.
- Browser text extraction is intentionally simple and bounded; JavaScript/CSS are ignored.
- AI text client supports OpenAI-compatible JSON and generic prompt payloads; provider-specific schemas may need config tuning.
- Web file manager has basic local-network endpoints without authentication by default.
- Clock NTP uses Europe/Paris timezone; manual time editing is minimal.
- Screen-off mode uses backlight brightness. Deep sleep is intentionally not used while background work may be active.
