# ABVx Mac Companion

The Companion is the desktop control surface for Pocket OS. Users should not need Terminal for routine device work.

## Source and mirror contract

Recommended local structure:

- `Cardputer Local/Music Source`
- `Cardputer Local/Books Source`
- `Cardputer Local/Exports/CardP SD Mirror`
- `Cardputer Local/Backups`

Rules:

- `Music Source` and `Books Source` keep human-friendly originals.
- `CardP SD Mirror` keeps only Cardputer-ready runtime files.
- The mounted SD should be treated as a deployment target, not as the source of truth.
- Companion import/export logic should operate between `Source -> Mirror -> SD`, not directly from arbitrary folders to firmware-facing storage.

## Architecture

```text
Mac UI
  -> Companion service/core
     -> mounted SD transport
     -> USB flash/build transport
     -> Connections Wi-Fi transport (after hardware validation)
```

`tools/abvx_companion.py` is the first reusable core, not the final user interface. Conversion, validation, naming, indexing, and transport must remain separate so the same operations can be called from CLI, local web UI, or a packaged macOS app.

For host-only staging use:

`tools/cardputer_local_pipeline.py`

It implements a repeatable source->mirror->deploy flow:

- `init` creates `Cardputer Local` source/mirror folders
- `sync-music` rebuilds prepared music mirror (with `INDEX.TXT`)
- `sync-books` rebuilds prepared books mirror (with `BOOKS.IDX`)
- `sync-all` rebuilds both sections
- optional `--deploy --sd /Volumes/NAME` pushes section mirrors directly to SD

## Recommended delivery path

1. Local Companion UI bound only to `127.0.0.1` (implemented in v0.1).
2. Package it as an ordinary macOS `.app` after the workflows stabilize.
3. Add Wi-Fi transport without changing conversion logic.

The local service approach is faster than starting with SwiftUI and can still provide drag-and-drop, file pickers, progress, logs, and one-button operations. It must never expose shell execution or arbitrary filesystem access over the network.

Run it with `./tools/abvx_companion_app.py`. It opens the browser automatically, detects prepared ABVx volumes and USB serial ports, and exposes only fixed import, build, flash, and time operations. Flash requires a currently detected port and explicit confirmation.

For normal use, open `tools/ABVx Companion.app` from Finder. The lightweight app bundle starts the same local service without opening Terminal.

## Initial screens

### Device

- Cardputer/SD detected state.
- SD capacity and library counts.
- USB serial port state.
- Firmware version when available.
- Synchronize time.

### Firmware

- Build current firmware.
- Select detected `/dev/cu.usbmodem*` port.
- Flash with one explicit confirmation.
- Stream concise progress and show actionable errors.
- Never flash automatically on app launch.

### Books

- Drag/drop TXT, EPUB, or FB2.
- Preview title, author, source format, chapter count, and output size.
- Convert to Reader-compatible UTF-8 TXT.
- Copy to mounted SD or later send through Connections.

### Music

- Drag/drop one file or a folder.
- Validate MP3 before copy.
- Preserve original title while assigning safe storage names.
- Show duplicates, rejected files, and copy progress.

### Files

- Browse the known Pocket OS folders.
- Export Notes, Voice, Timeline, and Habit logs.
- Import prepared content.
- Destructive operations require confirmation.

## Maintenance policy

When the device is connected for firmware maintenance:

1. Detect mounted SD and available device transport.
2. Offer backup/export of user data first.
3. Prefer offloading internal Voice recordings before any cleanup or reflash.
4. Only then allow cleanup, rebuild, or flash.

This matters because mounted-SD access alone does not include internal `/voice` storage. A pure SD workflow can back up the removable card, but it cannot preserve voice notes without a device-side export path.

## Product constraints

- Direct SD remains the first stable transport.
- Connections v3 stays experimental until large-transfer hardware tests pass.
- Build/flash uses the local ESP-IDF installation initially.
- The UI should show operations and results, not raw terminal output by default.
- PDF conversion is postponed until a reliable extraction layer is selected.
## macOS runtime

`ABVx Companion.app` keeps its executable backend and UI inside the app bundle. On launch it installs a runtime copy under `~/Library/Application Support/ABVx Companion`; the firmware checkout remains the source used by Build and Flash. macOS may request removable-volume access when a physical SD card is used.
