# ABVx Roadmap

Product direction: **Pocket OS** first, individual apps second.

ABVx should become a fast offline-first personal tool organized around actions:

```text
Capture / Remember / Read / Listen / Act / Reflect
```

Architecture:

- Product architecture: `docs/PRODUCT_ARCHITECTURE.md`
- Architecture decisions: `docs/ARCHITECTURE_DECISIONS.md`
- Browser architecture: `docs/BROWSER_ARCHITECTURE.md`
- Visual layer: `docs/VISUAL_LAYER_ARCHITECTURE.md`

## Phase 1: Stability baseline

Status: v0.2.0 baseline is hardware-tested; the current checkpoint adds Dashboard, Music/Time polish, battery diagnostics, and Habits v2.

Remaining work:

- Continue smoke testing Music -> Record -> Reader -> Notes -> Files after SD operations.
- Hardware-stress Connections v3 before treating large Wi-Fi upload as stable.
- Keep release checkpoints small and hardware-test them before tagging public binaries.

Acceptance:

- Music plays smoothly.
- Record creates, saves, plays, and deletes stable short voice notes.
- Reader/Notes/Files keep seeing SD after Music/Record operations.
- Settings SD reprobe works.

## Phase 2: Pocket OS foundation

Goal: turn the app set into a fast personal operating layer.

### 2.1 One Button Capture

Status: MVP implemented from launcher.

- `R`: start voice recording.
- `N`: new note.
- `M`: play selected music.
- `T`: timer shortcut later.
- Fn combinations remain optional/later.

Next acceptance:

- Hardware-test launcher shortcuts.
- Add Inbox logging after shortcuts are stable.

### 2.2 Universal Inbox

- Implemented as a persistent internal Timeline.
- Events queue in RAM and are committed from the main loop to `/voice/INBOX.LOG`.
- Voice saves, note saves, habit checks, reading opens, music plays, and timer events are supported.
- Inbox view/refresh never touches SD.

Acceptance:

- Captured activity appears in Timeline and survives reboot.
- No background SD writes during Music/Reader/Recorder operation.

### 2.3 Context Resume / Dashboard

- Store last useful state: book, track, note, recording, timer/habits summary.
- Dashboard shows Resume, time, battery, SD, routines progress, and keyboard shortcuts.

Acceptance:

- User can return to last book/track/note quickly.

### 2.4 Fast Dashboard

- Dashboard MVP implemented; later it can become an optional boot/default screen.

Acceptance:

- Boot gives useful state, not only app list.

### 2.5 Timeline

- Simple list/detail view over Inbox is implemented.
- Later: richer grouping by day/context after RTC/time sync.
- Use manual day/session time until real sync exists.

Acceptance:

- User can review today's captured activity.

## Phase 3: Offline app polish

Apps stay as implementation modules under Pocket OS actions.

1. Record v2
   - One hardware-verified 20-second Voice mode.
   - Playback progress.
   - Better duration display.
   - Robust storage failure messages.

2. Reader v2
   - Real English/Russian book tests.
   - Bookmark polish if test reveals gaps.
   - Last-book startup shortcut feeds Context Resume.

3. Notes v2
   - Rename notes.
   - Better line wrapping for long text.
   - Safer delete/restore pattern if useful.

4. Files v2
   - Details polish.
   - `/cardputer` transfer folder workflow.
   - Optional delete safeguards.

5. Time/Habits polish
   - Timer presets.
   - Durable daily history, rename/disable/restore, streaks, and 7D/30D/365D summaries.
   - Mac CLI date/time sync implemented; Companion UI and NTP remain future work.

## Phase 4: Connections / Transfer

Goal: Cardputer as SD transfer device via Wi-Fi AP and later Mac Companion.

Connections v3 staged upload is implemented and awaiting hardware stress validation.

Planned redesign:

- Keep current list/download endpoints.
- Validate staged/chunked upload across TXT, books, MP3, and interrupted sessions.
- Keep all SD writes in the main loop, never HTTP handlers.
- Add phone-friendly web UI.
- FATFS LFN is enabled for UTF-8 Music reads; keep conservative 8.3 naming for transfer writes until staged upload is hardware-proven.

Acceptance:

- Upload MP3/TXT/WAV-sized files without AP drop or SD loss.
- Failed upload must not corrupt SD state.
- `/cardputer` works as generic transfer folder.

## Phase 5: Mac Companion

Goal: local Mac tool for preparing and moving content to Cardputer SD.

- Import books and convert to clean TXT for `/books`.
- Import music and convert/rename to Cardputer-friendly MP3 for `/music`.
- Prepare browser favorite packages.
- Pull notes and recordings from SD.
- Sync time/config.
- Proxy AI requests.
- Firmware update later.
- Use BookOrbit as reference for library/import ideas, not as direct base. See `docs/MAC_COMPANION_REFERENCES.md`.

## Phase 6: Prepared-first Browser

Goal: text browser for a small set of favorite sites, not a graphical browser.

- Mac-prepared favorite packages first.
- Cardputer reads cached text + links.
- Offline shows saved pages.
- Online refresh updates known favorites later.
- Text Search is an adapter, not a full Google browser.
- AI Chat handles general questions.

## Phase 7: AI / Command Surface

AI is online-only.

Preferred route:

- Cardputer -> Mac Companion -> OpenAI -> Cardputer.

Later/fallback:

- Cardputer -> OpenAI API directly.

MVP:

- Text input in English, French, or translit.
- Text answer displayed on Cardputer.
- Save answer to Notes.
- Voice input/output later.

Allowed low-cost donor path:

- Evaluate tiny tool-calling models such as Needle only as a Mac Companion-side router over fixed Companion tools.
- Start with a tiny schema set: `sync_music`, `sync_books`, `sync_voice`, `sync_time`, `sd_status`, `prepare_browser_package`.
- Require explicit confirmation before any write, delete, flash, or network action.
- Prefer local execution for privacy/offline convenience, but keep the product contract tool-first rather than chat-first.
- Treat confidence/fallback as a routing aid, not as product permission to guess.
- Use the bounded implementation backlog in `docs/MAC_COMPANION.md` as the execution contract for any future Needle PoC.

No offline nano-LLM. No standalone Agent unless it becomes a real command/AI surface beyond launcher duplication.

That means:

- No firmware-resident assistant in the active roadmap.
- No open-ended local chat in Companion.
- No donor runtime adoption that bypasses the existing Companion core and fixed operations.

## Phase 8: Journey tracker

Hardware is available. The active sequence is GNSS Lab -> Journey Track -> Running Mode -> Voice Marks -> Mac GPX export. GNSS Lab reads checksum-valid NMEA over the Cap LoRa-1262 UART. An explicitly started Journey creates `/sdcard/journeys/J0001/TRACK.CSV`, writes the first valid point immediately, and then writes at most one point every 30 seconds.

Journey is allowed to run alongside Music. Its bounded tracking write is paused on Reader and Recorder screens rather than competing with their SD-sensitive operations.

Hardware gate status: Journey/Music navigation is proven, including `J` return and automatic track changes without leaving Journey. GNSS remains blocked at zero raw UART bytes from the Cap; resolve physical power/contact and then complete the outdoor fix/CSV gate before expanding tracking metrics.

Running Mode MVP is now layered over Journey with large distance, pace, and elapsed-time values. Pace is based on the 30-second Journey sample interval and should be hardware-evaluated before it is treated as a training metric.

LoRa remains a later direct computer/home-station relay after local Journey tracking is proven. It is not a mesh or Meshtastic feature.

Architecture: `docs/ADVENTURE_LORA_ARCHITECTURE.md`

## Final polish: Visual Layer

After core apps and companion workflows are stable, add Minimal/Art visual modes.

- Current UI remains default Minimal mode.
- Art mode is optional.
- Art mode is battery-gated and auto-falls back to Minimal when battery is low.
- Includes boot/sleep screens, transitions, cyberpunk accents, richer waveform/saver visuals.

## Out of scope

- Agro dashboards or market terminal positioning.
- Subscriptions or paid API model inside this firmware.
- Meshtastic integration.
- Full graphical browser.
- Offline fake AI / nano-LLM.
