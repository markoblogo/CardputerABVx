# First Real-Device Run

## v0.1.1 Hardware Usability Patch

- Default screen timeout is disabled (`screenTimeoutSec = 0`) for testing.
- Wake behavior is fixed: first key wakes only and is not forwarded.
- Navigation now works via red arrow punctuation keys (`;`,`,`,`.`,`/`) plus fallback `WASD`/`HJKL`.
- Launcher uses a one-tile carousel layout with visible index and number shortcuts.
- Music screen uses a cleaner track/status layout and visualizer.

## Required Hardware

- M5Stack Cardputer-Adv.
- USB-C data cable.
- microSD card, 4 GB or larger recommended.
- Computer with PlatformIO installed.
- Optional: headphones/speaker checks depending on your audio setup.

## Format The microSD Card

Format the card as FAT32. Use a simple volume name such as `CARDPUTER`.

On macOS, Disk Utility can erase the card as `MS-DOS (FAT)`.

## Prepare The SD Card

Recommended:

```sh
python3 tools/prepare_sdcard.py /Volumes/CARDPUTER
python3 tools/validate_sdcard.py /Volumes/CARDPUTER
```

Manual method: copy the contents of `sdcard_template/` to the SD card root.

Required folders:

```text
/music
/recordings
/notes
/books
/browser
/browser/bookmarks
/browser/saved_pages
/ai
/config
/logs
/tmp
```

## Where To Put Files

- Music: copy `.mp3` files to `/music`.
- Books: copy `.txt` files to `/books`.
- Notes: copy `.txt` files to `/notes`.
- Randomizer lists: edit `/config/randomizer_lists.txt`, one item per line. Lines starting with `#` are ignored.

## Configure Wi-Fi

Use the Network app on device to scan, enter a password, and save networks to `/config/wifi.json`.

You may also edit `/config/wifi.json` manually, but do not commit real passwords.

## Configure AI

Edit `/config/ai.json` on the SD card. Do not edit the committed template with real secrets.

Set:

```json
{
  "enabled": true,
  "endpoint": "https://your-endpoint.example/v1/chat/completions",
  "apiKey": "your-key",
  "model": "your-model",
  "provider": "openai_compatible",
  "systemPrompt": "You are a concise assistant.",
  "insecureTlsForTesting": true
}
```

Never commit real API keys.

## Configure Cast integration (Cardputer ↔ YTMamp)

If you run YTMamp on a PC in the same Wi‑Fi network, you can control it from Cardputer:

1. Open **Network** and connect Cardputer to the same LAN as the PC.
2. Open **Network** and edit `Cast Host` / `Cast Port`.
3. Set:
   - `cast host` — PC IP (`192.168.4.1` by default)
   - `cast port` — HTTP port (`3000` by default)
4. Save settings and open **Music**.
5. Press `GO` (short) to toggle between local player and CAST mode.
6. In CAST mode:
   - `SEL/ENT` — toggle
   - `RIGHT` — next
   - `LEFT` — prev
   - `UP` — play
   - `DOWN` — pause
   - No auth token required in this MVP.

Quick diagnostics on CAST screen:
- `Track ID` shows track identifier from `track.track_id`.
- `Err` shows last server-side error string if present.
- If desktop endpoint is unavailable, local SD playback remains available.

## Launcher Controls

- `1-0`: open app by index.
- `↑/↓/←/→`: next/previous app.
- `TAB`: next app.
- `WASD`/`HJKL`: fallback navigation.
- `GO` short: open.
- `GO` hold (`700ms`): back to launcher.

## Wake/Screen Behavior

- `screenTimeoutSec = 0` disables auto-off by default.
- `Input Test > ENT` can force screen-off for manual checks.
- Any key wakes the screen and does not get forwarded on the same event.

## Build

```sh
platformio run -e m5stack-cardputer-adv
```

## Upload

Connect the Cardputer-Adv over USB-C, then run:

```sh
platformio run -e m5stack-cardputer-adv -t upload
```

If upload fails, hold the boot/reset sequence required by your board and retry.

## Serial Monitor

```sh
platformio device monitor -e m5stack-cardputer-adv -b 115200
```

Exit with `Ctrl+]`.

## First Boot Checklist

1. Insert the prepared SD card.
2. Power on the Cardputer-Adv.
3. Confirm the main menu appears.
4. Open System Info and confirm SD is mounted.
5. Open Input Test and verify arrows, text keys, GO short press, and GO long press.
6. Open Notes and save a short note.
7. Open Reader with a `.txt` book.
8. Open Network and scan Wi-Fi.
9. Open Music with an `.mp3` file and test volume at low level first.
10. Open Recorder and save a short WAV.

## Expected Failure Messages

- `SD missing`: SD card is absent, unformatted, or not mounted.
- `No MP3 files in /music`: add `.mp3` files to `/music`.
- `No .txt books in /books`: add `.txt` files to `/books`.
- `Wi-Fi not connected`: connect from Network before Browser, AI, or Web File Manager.
- `API key missing`: configure `/config/ai.json` on the SD card.
- `HTTP error ...`: endpoint rejected the browser or AI request.
- `error: MP3 open/output failed`: audio path or file format needs device validation.

### Screen-off checks

- If backlight is off, press any key first.
- Expected serial message: `[Power] wake requested` followed by screen redraw.

## Known Limitations

- Browser is text-only. No JavaScript or CSS rendering.
- Web File Manager is local-network only and may be unauthenticated.
- AI depends on the endpoint schema and TLS behavior.
- Screen-off mode turns off backlight; deep sleep is not used.
- Audio hardware needs real-device tuning.

## Audio Safety Notes

M5Unified microphone and speaker paths may need exclusive hardware use.

- Stop Music before starting Recorder.
- Timer beep during recording may be suppressed or delayed.
- MusicApp and RecorderApp are the highest priority hardware validation targets.
