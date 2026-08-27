# ABVx Smoke Test

Use this checklist after flashing a release checkpoint.

## Boot and Navigation

1. Boot shows the ABVx splash.
2. Launcher opens with large menu text.
3. Up/Down moves selection.
4. OK opens the selected module.
5. GO returns to launcher.

## Listen / Music

1. Open `LISTEN`.
2. MP3 list appears quickly.
3. OK plays a track with waveform.
4. Up/Down changes volume.
5. Left/Right changes track.
6. `1` or `S` toggles shuffle.
7. In shuffle mode, play/skip through the full library and confirm no track repeats before the cycle completes.
8. Cyrillic and Hebrew physical filenames open and play directly; `prepare_music.py` remains an optional ASCII-normalization tool.
9. macOS `._*.MP3` sidecars and their 4 KB FAT aliases do not appear in the Music list.
10. Cyrillic/Hebrew titles render as glyphs rather than squares in Music List, Track Info, and Listening; long titles marquee without broken UTF-8 characters.
11. A malformed FAT name shows `Unsupported filename`; Music remains responsive and shuffle continues with the next playable track.
12. In Music List, Up on the first track wraps to the last track and Down wraps back to the first.
13. In Launcher, Up on the first app wraps to the last app and Down wraps back to the first.
14. With Journey active, press `J` from Music List, Track Info, or Listening; Journey reopens without stopping playback.
15. Let the current track finish while Journey is visible; the next track starts without switching the screen back to Music.
7. `2` or `I` opens `TRACK INFO`.
8. `2` or `P` inside Track Info runs the safe probe.

## Read / Write / Voice

1. `READ` opens small and large TXT books.
2. Line/page navigation works.
3. Speed mode opens with `1`.
4. `WRITE` creates, wraps, saves, reopens, edits, and deletes LAT notes.
5. Cyrillic notes open view-only.
6. `VOICE` records about 20 seconds, saves, plays, and deletes.
7. After Voice save/play, `READ`, `WRITE`, and `LISTEN` still show their SD files.

## Inbox / Persistence

1. Open a book, play a track, save a note, and save a Voice recording.
2. Return to launcher, then open `INBOX`.
3. Timeline shows `READ`, `LISTEN`, `NOTE`, and `VOICE` events.
4. Reboot and reopen `INBOX`; events remain visible.
5. `1` refreshes the internal journal without changing SD availability.

## GNSS / Journey

1. Open `JOURNEY` and inspect `UART B/L`, `NMEA`, `BAD`, and `SAT`. If `UART B` remains zero after 30 seconds, stop: power off and reseat the Cap before any outdoor fix test.
2. If UART bytes and checksum-valid NMEA counts increase, move outdoors; GNSS Lab should move from `NO FIX` to `FIX` and show coordinates, speed, altitude, and satellites.
3. Press OK to start; confirm `/sdcard/journeys/J0001/TRACK.CSV` is created with the CSV header and a first valid point.
4. Leave the device on the Journey or Running screen for at least 30 seconds; confirm a later row is appended, not more than once per 30 seconds.
5. While Journey is active, GO/BACK keeps the session screen open and shows `STOP FIRST`.
6. Press `M`, start Music, then press `J`; confirm playback continues, automatic track changes remain on Journey, and point count increases across a logging interval.
7. Open Reader or Recorder while the session is active; confirm no Journey row is written until returning to an allowed Journey, Launcher, Dashboard, or Music screen.
8. Return to Journey and press OK STOP; confirm the track file closes and no further rows are appended.

## Running Mode

1. Start a Journey and press Right (`/`) to open `RUNNING`.
2. Confirm large distance, pace, and elapsed-time fields refresh while the session is active.
3. Press `M` to open Music without ending the Journey; GO/BACK from Running returns to Journey; OK STOP ends the session.

## Utilities

1. `TIME` stopwatch, timer preset, and alarm work.
2. `FILES` shows `TRANSFER`, known files open, unsupported files show `FILE INFO`.
3. `ROUTINES` toggles items, `1` advances day, stats/manage work.
4. `SETTINGS` shows SD/BAT/Transfer password, About, and SD reprobe.
5. `D` or `0` opens Dashboard; OK resumes, `1/2/3` shortcuts work.

## Battery / Charging

1. With USB disconnected for 30-60 seconds, record the displayed battery percentage.
2. Connect USB with the top power switch `ON`; do not interpret an immediate voltage rise as a measured state of charge.
3. Cardputer ADV cannot expose reliable charging status or battery current. Treat any `+`/animation as inferred only.
4. After a charging interval, disconnect USB for 30-60 seconds and confirm the settled percentage increased.
5. Repeat once without Music and Cap, then with each enabled separately, to identify a load that exceeds available charging headroom.

## Transfer

1. `TRANSFER` starts AP.
2. Network `ABVX-Cardputer` appears.
3. Password is `cardputer`.
4. `http://192.168.4.1/api/ping` returns `ping ok`.
5. `/api/list?path=/music` lists files.
6. `/api/download?path=/notes/NOTE0001.TXT` downloads when the file exists.
7. Connections v3: upload TXT, 500-900 KB book, 5-10 MB MP3, then interrupt one upload; verify AP and SD remain available after every case.
8. Time sync: run `python3 tools/cardputer_time_sync.py sync`, confirm `OK TIME APPLIED`, then compare Time with the Mac clock.
