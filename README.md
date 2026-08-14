ABVx is a **Pocket OS** for M5Stack Cardputer ADV: a fast offline-first personal tool for capture, memory, reading/listening, routines, and transfer.

It is not just a launcher or a set of apps. The product goal is to minimize friction for common actions:

```text
Capture -> Remember -> Read -> Listen -> Act -> Reflect
```

The existing apps are implementation modules behind those actions.

## Product direction

Current firmware is the technical baseline for ABVx Pocket OS.

Core ideas:

- **One Button Capture**: start voice/text capture with one shortcut.
- **Universal Inbox**: notes, voice, habits, bookmarks, and actions flow into one log.
- **Timeline**: a simple life/activity journal built from captured events.
- **Context Resume**: resume last book, track, note, timer, or habit state.
- **Fast Dashboard**: boot into status + resume actions, not only an app list.
- **Zero Cursor Philosophy**: use physical keys directly where possible.
- **Progressive Apps**: simple first screen, advanced options later.
- **Minimal / Art mode**: productive default UI, optional battery-gated visual mode.

Architecture: [`docs/PRODUCT_ARCHITECTURE.md`](docs/PRODUCT_ARCHITECTURE.md)
Decisions: [`docs/ARCHITECTURE_DECISIONS.md`](docs/ARCHITECTURE_DECISIONS.md)
Decision receipts: [`docs/decision-receipts/README.md`](docs/decision-receipts/README.md)
Roadmap: [`docs/ROADMAP.md`](docs/ROADMAP.md)
Mac Companion: [`docs/MAC_COMPANION.md`](docs/MAC_COMPANION.md)

## Current status

Current baseline: **v0.2.2-test Pocket OS checkpoint**.

Tested on real Cardputer ADV hardware.

Stable baseline:

- Boot: ABVx splash and large monochrome launcher.
- Listen/Music: SD MP3 player with waveform, volume, next/prev, non-repeating shuffle, MAX volume, buffered playback, sorted library, Unicode-safe marquee/glyphs, track info/probe, and direct FatFS streaming.
- Voice/Record: one RAM-first 20-second WAV voice-note mode stored in internal SPIFFS, with save/play/delete and waveform.
- Read/Reader: small and large TXT books, streaming reader, English/Russian display, `1W / 2W / LINE` speed reading, persistent bookmarks.
- Write/Notes: LAT/plain text create/open/edit/delete. Cyrillic notes are view-only.
- Time: manual clock, stopwatch, timer presets, alarm, and Connections-based Mac time sync.
- Files: SD browser, `TRANSFER` folder, file opening, unsupported-file info, delete confirmation.
- Routines/Habits: larger daily checklist, manual next day, manage screen, restore disabled habits, 7D/30D/365D stats.
- Decide/Randomizer: `YES / NO / MB`.
- Inbox/Timeline: persistent internal log of the latest 64 confirmed events; no SD access.
- Dashboard/Settings: Resume dashboard, current context, battery/low-voltage diagnostics, Transfer password, theme, sound, timeout, power preset, SD reprobe, About.
- Transfer/Connections v3: Wi-Fi AP list/download plus staged, main-loop-owned upload.
- Mac Companion Core: direct-SD status/layout, validated MP3 import, TXT-to-UTF-8 book import, and clock sync.

Next desktop layer: local Companion UI and packaged macOS app over the existing conversion core. Postponed: Companion Wi-Fi transport, browser, AI, and Bluetooth transfer.

Detailed status: [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md)
Changes: [`CHANGELOG.md`](CHANGELOG.md)
Smoke test: [`docs/SMOKE_TEST.md`](docs/SMOKE_TEST.md)

## Controls

Global:

- Up / Down: move selection.
- Left / Right: page or switch mode.
- OK / Enter: open, start, save, confirm.
- GO / Back: return or stop.
- Backspace: delete where supported.
- `1`: app-specific shortcut.

App highlights:

- Listen: OK play/stop, Up/Down volume, Left/Right track, `1` shuffle, `2`/`I` track info, `2`/`P` probe inside track info.
- Read: Up/Down line, Left/Right page, `1` speed mode for any supported book, OK pause/resume in speed mode.
- Write: `1` new/edit, Backspace delete from list.
- Voice: OK starts `NEW REC`, OK/GO stops and saves, OK plays a saved recording. Auto-save at 20 seconds.
- Routines: OK toggles, `1` next day, Right/`S` stats, Left/`M` manage.
- Inbox: OK opens event detail, `1` reloads the internal Timeline.
- Transfer: OK starts AP, GO stops it; list/download are stable, upload is small-file only.

One Button Capture from launcher:

- `R`: start voice recording.
- `N`: new text note.
- `M`: play selected music.
- `2` or `S`: resume last used context in the current session.
- `D` or `0`: open session dashboard.

## SD layout

Transfer APIs still prefer 8.3-safe destination names. Music displays UTF-8 long filenames and streams through direct FatFS, trying the short alias and original LFN. A malformed FAT directory name is reported as `Unsupported filename` and should be renamed on Mac.

```text
/sdcard/music/A.MP3
/sdcard/books/EN1.TXT
/sdcard/books/RU1.TXT
/sdcard/notes/NOTE0001.TXT
/sdcard/habits/HABITS.TXT
/sdcard/habits/LOG.TXT
/sdcard/habits/STATE.TXT
/sdcard/CARDPTR/CONFIG.TXT
/sdcard/CARDPTR/READER.TXT

internal SPIFFS:
/voice/REC00001.WAV
/voice/INBOX.LOG
```

## Connections MVP

Wi-Fi AP:

- SSID: `ABVX-Cardputer`
- Password: `cardputer`
- URL: `http://192.168.4.1`

Useful endpoints:

```sh
curl http://192.168.4.1/api/ping
curl http://192.168.4.1/api/status
curl "http://192.168.4.1/api/list?path=/music"
curl "http://192.168.4.1/api/download?path=/notes/NOTE0001.TXT"
curl http://192.168.4.1/api/write-test
```

Clock sync from the connected Mac:

```sh
python3 tools/cardputer_time_sync.py sync
python3 tools/cardputer_time_sync.py status
```

The CLI sends Unix time plus the Mac's current UTC offset. The HTTP handler only queues the update; the firmware main loop applies system time and the displayed clock. A full power-off still requires another sync.

Staged upload, up to 32 MB:

```sh
python3 tools/cardputer_upload.py ./BOOK.TXT /books/BOOK.TXT
python3 tools/cardputer_upload.py ./track.mp3 /music/track.mp3
```

The CLI uses `upload-begin/chunk/finish/abort`. HTTP handlers copy one bounded chunk; only the firmware main loop writes `ABVXUP.TMP` to SD. Finish verifies and renames it atomically. Existing files are never overwritten. `/cardputer` appears as `TRANSFER` in Files.

## Preparing a music library

```sh
python3 tools/prepare_music.py ~/Downloads/Music /Volumes/CARDPUTER/music
python3 tools/prepare_music.py --in-place /Volumes/CARDPUTER/music
```

This conversion is optional: current firmware can open UTF-8 FAT long filenames directly. Use it when portable ASCII storage names are desirable.

Optional preparation stores tracks as FAT-safe `M001.MP3`, `M002.MP3`, etc. Transliterated display titles are kept in `INDEX.TXT`. Native Cyrillic/Hebrew filenames can also be displayed and played directly. Shuffle walks one complete shuffled playlist before any track repeats.

Recommended workflow is now:

- Keep a human-friendly Mac source library such as `~/Downloads/Cardputer Local`.
- Put original files into `Music Source` and `Books Source`.
- Build a prepared runtime mirror under `Exports/CardP SD Mirror`.
- Copy only the prepared mirror to the mounted SD.

In other words:

- Mac = source of truth
- SD = prepared runtime copy for Cardputer

Host-only pipeline (no flash/debugging needed):

```sh
python3 tools/cardputer_local_pipeline.py init
python3 tools/cardputer_local_pipeline.py sync-music --deploy --sd /Volumes/CARDPUTER
python3 tools/cardputer_local_pipeline.py sync-books --deploy --sd /Volumes/CARDPUTER
python3 tools/cardputer_local_pipeline.py status
```

## Mac Companion Core

The first Companion layer works directly with a mounted SD and uses only the Python standard library:

```sh
python3 tools/abvx_companion.py --sd /Volumes/CARDPUTER init
python3 tools/abvx_companion.py --sd /Volumes/CARDPUTER status
python3 tools/abvx_companion.py --sd /Volumes/CARDPUTER add-music ./track.mp3
python3 tools/abvx_companion.py --sd /Volumes/CARDPUTER add-book ./book.epub ./other.fb2
python3 tools/abvx_companion.py sync-time
```

When exactly one mounted volume already contains at least two ABVx folders, `--sd` may be omitted. Music is validated and stored as `Mxxx.MP3` with its original UTF-8 title in `INDEX.TXT`. TXT, EPUB, and FB2 books are converted to Reader-compatible UTF-8 `Bxxxx.TXT`; EPUB follows its package spine, while FB2 follows body sections. `BOOKS.IDX` preserves title, source format, and author. PDF remains intentionally unsupported. Files are copied through temporary files and renamed only after flush.

Operational policy for maintenance/reflash:

- Before cleanup or firmware reflash, Companion should first offer an export/offload step for user data.
- Voice recordings stored in internal `/voice` are higher priority than SD cleanup because they are not preserved by simply mounting the SD card on macOS.
- SD-only workflows can back up `music`, `books`, `notes`, and other mounted folders, but they cannot capture internal `/voice` data without a device-side export path.

Launch the local Companion UI:

```sh
./tools/abvx_companion_app.py
```

Or open `tools/ABVx Companion.app` in Finder to launch it without Terminal.

It opens `http://127.0.0.1:8765` and provides automatic SD/USB status, drag-and-drop Books and Music import, Time Sync, firmware Build, and guarded Flash. It binds only to localhost, uses a per-launch request token, and accepts only fixed operations. The current version uses the locally installed ESP-IDF 5.4.2 toolchain.

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
cd /Volumes/Work/Work/cardputer-abvx-minimal
. ~/esp/esp-idf-v5.4.2/export.sh
idf.py build
```

Firmware:

```text
build/cardputer-abvx-minimal.bin
```

Flash:

```sh
ls /dev/cu.usbmodem*
idf.py -p /dev/cu.usbmodem101 flash
```

## Smoke test

1. Boot: ABVx splash and launcher.
2. Music: play `A.MP3`, check smooth sound/waveform, stop.
3. Record: create short note, confirm `Record saved`, play it back, delete test file.
4. Reader: open both a small TXT and a large English/Russian TXT, scroll by line/page, test `1W / 2W / LINE`, then exit/reopen and confirm the bookmark.
5. Notes: create LAT note, edit it, delete it; Cyrillic note should be view-only.
6. Time: stopwatch, timer, alarm sound.
7. Files: browse SD, open TXT/MP3/WAV, open unsupported file info.
8. Habits: toggle habit, next day, stats.
9. Settings: SD reprobe, About, config/theme.
10. Connections: AP starts, `/api/ping`, list, download, write-test.
11. Dashboard: `D` or `0` opens Resume/status; Settings About shows the current ABVx Pocket OS build.

## HTML Artifacts For BrowserApp

For pages that should remain readable in the text-first BrowserApp flow, use the local single-file HTML contract in [docs/html-artifact-contract.md](docs/html-artifact-contract.md).

Starter templates:

- [templates/html/cardputer-status-report.html](templates/html/cardputer-status-report.html)
- [templates/html/cardputer-reference-page.html](templates/html/cardputer-reference-page.html)

Real artifact:

- [docs/browserapp-troubleshooting-page.html](docs/browserapp-troubleshooting-page.html)

SD-ready saved page:

- [sdcard_template/browser/saved_pages/browserapp-troubleshooting.html](sdcard_template/browser/saved_pages/browserapp-troubleshooting.html)
