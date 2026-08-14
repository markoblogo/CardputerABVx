# ABVx Project Status

Current baseline: **v0.2.2-test Pocket OS checkpoint**.

ABVx is an offline-first Cardputer ADV Pocket OS for fast capture, memory, reading/listening, routines, and transfer.

## Working hardware-verified features

- Launcher/Dashboard: large monochrome launcher, Resume, current time, routines progress, current context, and battery/low-voltage diagnostics.
- Music: buffered 16 kHz mono MP3 playback, waveform, MAX volume, non-repeating shuffle cycles, Unicode title glyphs/marquee, track info/probe, and direct FatFS streaming through short alias/LFN candidates.
- Record: one hardware-verified 20-second 8 kHz/8-bit WAV mode, RAM-first capture, waveform, playback, delete confirmation.
- Reader: `.TXT` books from `/sdcard/books`, normal reading, speed mode, persistent bookmark state.
- Notes: `.TXT` notes from `/sdcard/notes`, LAT/plain create/edit/delete, Cyrillic view-only.
- Files: SD browser, `TRANSFER` folder for `/sdcard/CARDPTR`, known file opening, unsupported file info, delete confirmation.
- Time: manual clock, stopwatch with fractions, timer presets, alarm sound, and hardware-verified Connections time sync with a Mac CLI.
- Habits: larger routines list, daily checks, manual next-day rollover, rename, confirmed disable, restore disabled habits, streaks, and 7D/30D/365D summaries.
- Randomizer: simple yes/no/maybe decision utility.
- Settings: theme, sound, timeout, power preset, SD/BAT/Transfer password status, SD reprobe, About.
- Transfer/Connections v3: Wi-Fi AP with fixed password `cardputer`, ping/status/list/download and staged upload up to 32 MB; hardware stress validation is pending.
- Mac Companion Core v0.2: dependency-free direct-SD status/init, MP3 validation/import, TXT/EPUB/FB2 conversion to Reader UTF-8, metadata index, and unified clock sync.
- Companion product direction: replace routine Terminal work with a local Mac UI for device status, drag/drop content preparation, one-button build/flash, files, and time sync; package as `.app` after workflows stabilize.
- Mac Companion UI v0.1: localhost-only dashboard with automatic SD/USB detection, drag/drop Books/Music, Time Sync, background Build, confirmed Flash, progress, and operation log.
- Current content workflow contract: Mac source library -> prepared `CardP SD Mirror` -> mounted SD. Safe filenames and indexes belong in the prepared mirror, not in the source library.

## Product reframing

The apps are no longer the top-level product idea. They are implementation modules behind Pocket OS actions:

- Capture: Record, Notes, future One Button Capture.
- Remember: Notes, Habits, future Inbox/Timeline.
- Read: Reader, prepared Browser pages.
- Listen: Music, Record playback.
- Act: Time, Randomizer, Files.
- Reflect: Habits stats, future Timeline/Dashboard.

## Safety decisions

- Large Wi-Fi upload now uses a bounded RAM slot and main-loop `.TMP` writes. Treat it as test-stage until TXT/book/MP3/interruption hardware checks pass.
- Music displays heap-backed UTF-8 FAT long names and streams files through direct FatFS. Malformed FAT names remain unsupported and require host-side rename; transfer destinations still prefer 8.3 names.
- Agent is hidden/postponed until it becomes more than a duplicate launcher.
- Browser, AI, Companion GUI, and Companion Wi-Fi transport are planned but must not destabilize offline apps.

## Known limitations

- Recorder is RAM-first because Cardputer ADV has no PSRAM and live SD writes during microphone capture are unsafe.
- Battery percentage is an estimate from Cardputer/M5 power readings; low or unstable samples may show diagnostic voltage instead of a false percentage.
- Clock resets after full power-off; manual Mac sync is available through Connections, while persistent RTC/NTP is not.
- Notes editor is LAT/plain text only; Cyrillic files are view-only.
- Reader Cyrillic is custom/best-effort; French/Ukrainian polish is postponed.
- Connections v3 is ready for controlled large-transfer hardware testing.
- Voice and Inbox use the dedicated internal SPIFFS partition. SD remains the bulk user-content store.
- Because Voice data is internal, Companion maintenance/reflash flow should treat recording offload as a first-class preflash step, not as optional SD cleanup.
- Inbox/Timeline persists the newest 64 confirmed events and writes only from the main loop on safe idle screens.

## Architecture

Architecture decisions are tracked in `docs/ARCHITECTURE_DECISIONS.md`.

Key decisions: Pocket OS action-first framing, AI online-only, no standalone Agent for now, Browser prepared-first, Mac Companion handles heavy conversion/sync, and Connections v3 large upload remains test-stage until hardware stress validation.
