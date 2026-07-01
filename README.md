# CardputerABVx Minimal Firmware

Minimal ABVx firmware for M5Stack Cardputer ADV.

Current focus: MP3-first offline shell with Music, Record, Reader, and Notes MVPs.

## Current working version

Confirmed on real Cardputer ADV:

- ABVx boot splash.
- Large monochrome launcher UI with battery percentage widget.
- Music app:
  - scans `/sdcard/music` for `.MP3` / `.mp3` files;
  - plays MP3 files from SD;
  - waveform visualization;
  - `OK` / `GO` stop playback;
  - Up / Down volume control: `MUTE`, `MID`, `LOUD`;
  - Left / Right track navigation;
  - `1` shuffle toggle;
  - EOF auto-next when multiple tracks exist.
- Reader app:
  - scans `/sdcard/books` for `.TXT` / `.txt` files;
  - opens English/Russian UTF-8 text best-effort;
  - line and page scrolling;
  - in-RAM bookmarks restore the last line while the device remains on;
  - speed-reading mode: 1 word, 2 words, or line at 350-1000 WPM.
- Notes app:
  - stores notes in `/sdcard/notes`;
  - first list item `NEW NOTE` creates a new plain text note;
  - editor supports `LAT` mode and `RU` translit-save mode toggled by `1`;
  - saves files as `NOTE0001.TXT`, `NOTE0002.TXT`, etc.;
  - opens saved notes using the same readable text viewer.
- Files app:
  - browses SD folders and known file types;
  - shows SD free/used storage status;
  - opens `.MP3`, `.TXT`, and `.WAV` files through existing Music/Reader/Notes/Record paths;
  - delete/rename are intentionally not implemented yet.
- Randomizer app:
  - rolls `YES`, `NO`, or `MB` using `esp_random()`;
  - does not store history yet.
- Time app:
  - includes Clock, Stopwatch, Timer, and Alarm modes;
  - Left / Right switches mode;
  - Stopwatch shows tenths of seconds;
  - Timer and Alarm support hour/minute/second fields selected with `1`;
  - Clock is manual/runtime-only for now.
- Record app:
  - stores recordings in `/sdcard/rec`;
  - first list item `NEW REC` starts recording;
  - `OK` / `GO` stops and saves;
  - saves 16-bit mono 16 kHz WAV files as `REC0001.WAV`, `REC0002.WAV`, etc.;
  - plays saved recordings;
  - waveform visualization during record and playback.

## SD card layout

Use FATFS 8.3-safe names for now.

```text
/sdcard/music/A.MP3
/sdcard/music/T01.MP3
/sdcard/books/EN1.TXT
/sdcard/books/RU1.TXT
/sdcard/notes/NOTE0001.TXT
/sdcard/rec/REC0001.WAV
```

Long filenames and long folder names are not reliable yet.

## Build

```sh
cd /Volumes/Work/Work/cardputer-abvx-minimal
. ~/esp/esp-idf-v5.4.2/export.sh
idf.py build
```

Firmware output:

```text
build/cardputer-abvx-minimal.bin
```

## Flash

Find the current USB modem port if needed:

```sh
ls /dev/cu.usbmodem*
```

Flash:

```sh
idf.py -p /dev/cu.usbmodem101 flash
```

Replace `/dev/cu.usbmodem101` with the actual port.

## Hardware test checklist

### Music

1. Open Music.
2. Select `A.MP3` or another MP3.
3. Press `OK`.
4. Confirm sound, waveform, and `PLAYING` screen.
5. Test `OK` / `GO` stop.
6. Test Up / Down volume.
7. Test Left / Right track change.
8. Test `1` shuffle toggle.

### Reader

1. Open Reader.
2. Select `EN1.TXT` or `RU1.TXT`.
3. Press `OK` to read.
4. Test Up / Down line scroll.
5. Test Left / Right page scroll.
6. Press `1` for speed-reading mode.
7. Test `OK` pause/resume, Up / Down WPM from 350 to 1000, Left / Right mode.
8. Press `GO` to return to normal read mode, then list.

### Notes

1. Open Notes.
2. Select `NEW NOTE`.
3. Press `OK`.
4. Type a short note. Press `1` to toggle LAT/RU translit mode if needed.
5. Press `OK` to save.
6. Confirm `NOTE0001.TXT` appears.
7. Press `OK` on the saved note to read it.

### Files

1. Open Files.
2. Navigate folders with Up / Down and OK.
3. GO backs out of folders, then launcher.
4. Open `.TXT`, `.MP3`, and `.WAV` files if available.
5. Confirm unknown files are hidden or not opened.

### Time

1. Open Time.
2. Test Left / Right mode switching: Clock, Stop, Timer, Alarm.
3. Stopwatch: `OK` start/stop, `1` reset, tenths visible.
4. Timer: `1` selects hour/min/sec, Up / Down changes selected field, `OK` start/stop, `1` reset when running/done.
5. Clock: `1` selects hour/min/sec, Up / Down adjusts manually.
6. Alarm: `1` selects hour/min/sec, Up / Down adjusts, `OK` arms/disarms or stops ringing.

### Randomizer

1. Open Random.
2. Press `OK`.
3. Confirm result changes between `YES`, `NO`, and `MB`.
4. Press `GO` to return to launcher.

### Record

1. Open Record.
2. Select `NEW REC`.
3. Press `OK`.
4. Speak for a few seconds.
5. Press `OK` or `GO` to save.
6. Confirm `REC0001.WAV` appears.
7. Press `OK` on the WAV file.
8. Confirm playback and waveform.

## Known limitations

- Reader/Notes text support is MVP only: English/Russian best-effort, in-RAM Reader bookmarks only, no persistent saved position, Reader/Notes include a compact Cyrillic bitmap fallback for Russian/Ukrainian text; French accents are not supported yet.
- Notes input supports latin text plus RU transliteration save mode; the edit screen shows latin translit because the current large font does not render Cyrillic. Punctuation is still limited by Cardputer arrow-key mappings.
- Recordings use `/sdcard/rec` instead of `/sdcard/recordings` because FATFS long filenames are not enabled yet.
- Music playback is chunk/blocking-based; controls may have small latency.
- Files MVP is read/open only; delete and rename are postponed.
- Future Transfer/Connections app should provide Wi-Fi AP + HTTP file manager for moving arbitrary files to/from SD without removing the card.
- Time block is implemented for this stage: manual runtime clock, stopwatch, timer, and alarm. Full power-off resets time; future Mac sync should set time once during sync.
- Screen-off playback/power optimization is not finalized yet.
- Dev diagnostics were removed from the normal build after proving MP3 decode/speaker path.
