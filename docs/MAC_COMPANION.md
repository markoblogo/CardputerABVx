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

### Export status

- Constant local backup is maintained in `~/ABVxCompanionBackup`:
  - `Tracks/`
  - `Notes/`
  - `Voice/`
  - `.state/sync-status.json`
- The UI exposes **Last / Pending / Failed / Done** state for:
  - Tracks (import)
  - Notes (pull)
  - Voice (pull)
- This is non-invasive to firmware and keeps local visibility of recovery operations.
- Notes and Voice pulls support optional post-sync cleanup (`delete_after`) from the same file system transaction.
- Sync panel has a compact heartbeat marker showing when any sync is actively in pending state.

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

## Bounded AI option

If AI is added to Companion later, the first acceptable shape is a narrow command router over existing Companion operations.

Rules:

- AI may choose only from fixed tool schemas already implemented by Companion core.
- First candidate commands are `sync_music`, `sync_books`, `sync_voice`, `sync_time`, `sd_status`, and prepared-browser packaging.
- Every mutating action still requires explicit user confirmation in the UI.
- The router must return structured intent plus confidence; low-confidence requests fall back to ordinary buttons/forms.
- It must never gain arbitrary shell access, arbitrary filesystem access, or direct firmware control outside existing guarded flows.

This keeps AI as a thin intent layer over `tools/abvx_companion.py`, not as a second product runtime.

## Needle PoC backlog

### 1. Needle PoC contract

- Scope: Mac Companion only.
- Goal: accept short natural-language commands and map them to fixed Companion tools.
- Non-goals: firmware-side AI, open-ended chat, autonomous background actions, arbitrary shell/file access.
- Output contract: `{intent, arguments, confidence, summary}`.
- Safety contract: any write, delete, flash, sync, or network action still requires explicit confirmation.

### 2. Companion tool schema set

- First schema set stays intentionally small.
- Required tools: `sd_status`, `sync_time`, `sync_music`, `sync_books`, `sync_voice`.
- Optional sixth tool: `prepare_browser_package` when that flow exists as a fixed Companion operation.
- Each tool schema should use bounded enums/strings instead of free-form paths where possible.
- Tool implementations must call existing Companion core operations rather than adding a second execution path.

### 3. Confirmation and fallback UX

- Read-only intents may run directly and show a concise result card.
- Mutating intents must stop at a confirmation card with resolved arguments.
- Low-confidence or ambiguous results must fall back to ordinary buttons/forms.
- UI copy should explain what will happen in Companion terms, not model terms.
- If the router fails or is disabled, the Companion remains fully usable through the current UI.

### 4. Acceptance checklist

- A real user can complete common commands faster than through manual navigation.
- No destructive or mutating action runs without explicit confirmation.
- Incorrect or low-confidence routing degrades to safe fallback rather than guessing.
- `Source -> Mirror -> SD` invariants remain unchanged.
- Removing the PoC does not break any existing Companion workflow.

## Needle PoC technical spec

This PoC is a local intent router for Companion. It does not execute arbitrary commands and it does not replace the existing UI.

### Request shape

`POST /api/intent/resolve`

```json
{
  "text": "sync music to sd",
  "context": {
    "sd_detected": true,
    "usb_detected": false,
    "voice_pending": true,
    "browser_package_enabled": false
  }
}
```

Rules:

- `text` is required plain text.
- `context` is optional and contains only bounded Companion state flags.
- No raw filesystem paths are accepted from the model-facing request.
- The resolver may accept bounded mixed-language shortcuts, including simple Russian, English, and translit variants, but only when they map onto the same fixed intent enum.
- The adapter backend is replaceable. Current default is `rule_based`; a future `needle_stub`/`needle` backend must preserve the same request and response contract.

### Resolve response

```json
{
  "status": "ok",
  "intent": "sync_music",
  "arguments": {
    "target": "sd"
  },
  "confidence": 0.93,
  "summary": "Sync prepared music mirror to the mounted SD card.",
  "requires_confirmation": true,
  "fallback_reason": null
}
```

Response fields:

- `status`: `ok`, `fallback`, or `reject`.
- `intent`: fixed enum or `null`.
- `arguments`: validated object for the selected intent.
- `confidence`: float in `0.0..1.0`.
- `summary`: short human-readable explanation for the UI.
- `requires_confirmation`: true for any mutating operation.
- `fallback_reason`: bounded string or `null`.
- `adapter`: backend name that produced the result.
- `adapter_meta`: optional backend descriptor for UI/debug visibility.
- Companion UI may surface the active adapter backend so the user can tell whether routing is `rule_based`, `needle_stub`, or a future real backend.

### Fallback and reject shapes

Low confidence or ambiguity:

```json
{
  "status": "fallback",
  "intent": null,
  "arguments": {},
  "confidence": 0.41,
  "summary": "Open the Music sync panel.",
  "requires_confirmation": false,
  "fallback_reason": "low_confidence"
}
```

Out of scope request:

```json
{
  "status": "reject",
  "intent": null,
  "arguments": {},
  "confidence": 0.0,
  "summary": "This request is outside the Companion command set.",
  "requires_confirmation": false,
  "fallback_reason": "out_of_scope"
}
```

### Intent enum v0

- `sd_status`
- `sync_time`
- `sync_music`
- `sync_books`
- `sync_voice`
- `prepare_browser_package`

### Tool argument schemas v0

`sd_status`

```json
{
  "type": "object",
  "properties": {},
  "additionalProperties": false
}
```

`sync_time`

```json
{
  "type": "object",
  "properties": {
    "target": { "type": "string", "enum": ["device"] }
  },
  "required": ["target"],
  "additionalProperties": false
}
```

`sync_music`

```json
{
  "type": "object",
  "properties": {
    "target": { "type": "string", "enum": ["sd"] }
  },
  "required": ["target"],
  "additionalProperties": false
}
```

`sync_books`

```json
{
  "type": "object",
  "properties": {
    "target": { "type": "string", "enum": ["sd"] }
  },
  "required": ["target"],
  "additionalProperties": false
}
```

`sync_voice`

```json
{
  "type": "object",
  "properties": {
    "delete_after": { "type": "boolean" }
  },
  "required": ["delete_after"],
  "additionalProperties": false
}
```

`prepare_browser_package`

```json
{
  "type": "object",
  "properties": {
    "profile": { "type": "string", "enum": ["favorites"] }
  },
  "required": ["profile"],
  "additionalProperties": false
}
```

### Execution contract

- `POST /api/intent/resolve` only resolves intent; it never executes it.
- Companion UI renders the summary and any resolved arguments.
- Execution continues only after explicit user confirmation for mutating intents.
- Confirmed execution must call the same fixed handlers already used by the current UI.
- The resolver may not fabricate unavailable state; it can only use supplied `context` and existing Companion detection.
- Swapping adapters may change how the intent is inferred, but not what intents exist, how arguments are validated, or how execution is confirmed.

### Stub backend contract

`needle_stub` is allowed to expose a more runtime-like envelope while still delegating actual inference to bounded local rules.

- `adapter_request` may describe model-facing input such as text, bounded context, and allowed intents.
- `adapter_response` may describe model-like output such as chosen intent, arguments, confidence, and status.
- These fields are diagnostic and compatibility-oriented only; they do not create a second execution path.
- A future real backend should preserve these outer shapes so the UI and confirm flow do not need redesign.

### Real Needle backend contract

The real backend uses the official Needle Python package and the one-turn `complete()` API.

- Install path: `pip install cactus-needle`
- Runtime selector: `ABVX_INTENT_ADAPTER=needle`
- Optional confidence gate: `ABVX_INTENT_CONFIDENCE=0.75`
- Optional custom weights: `ABVX_INTENT_NEEDLE_WEIGHTS=/path/to/model.cact`
- Optional persisted tool index: `ABVX_INTENT_NEEDLE_TOOL_INDEX=~/Library/Application Support/ABVx Companion/needle-tools.idx`

Backend rules:

- Companion passes raw JSON-schema tools, not Python side-effecting functions.
- Needle is used only for intent selection and argument filling; Companion still owns execution.
- Empty calls become `out_of_scope`.
- Confidence below the configured threshold becomes `low_confidence`.
- Runtime/import/init failures become `backend_unavailable` and must degrade safely in the UI.
- System facts must be passed as a single Needle `system` string and refreshed when host-side SD or backup state changes.

### Host-side runtime wiring

Current practical runtime path:

- Run Companion itself under a Python interpreter that has `cactus-needle` installed.
- For Terminal launch, use that interpreter directly.
- For `ABVx Companion.app`, the launcher now prefers:
  - `ABVX_COMPANION_PYTHON` when explicitly set,
  - `~/Library/Application Support/ABVx Companion/.venv/bin/python3`,
  - then the normal host `python3`.

This keeps the Companion surface unchanged while allowing a dedicated host-side Needle runtime.

### Confirmation card contract

The confirmation UI should show:

- resolved action name in Companion terms,
- resolved arguments,
- current device/SD preconditions,
- one primary confirm action,
- one cancel/fallback action.

Example:

```json
{
  "action_label": "Sync Music to SD",
  "summary": "Sync prepared music mirror to the mounted SD card.",
  "arguments": {
    "target": "sd"
  },
  "preconditions": {
    "sd_detected": true
  }
}
```

### Error semantics

- Use `fallback` when the request is plausibly within scope but unresolved.
- Use `reject` when the request is outside the bounded command set.
- Never convert missing prerequisites into guessed arguments.
- Never auto-run on partial matches such as `maybe sync something`.
- Keep synonym support bounded and explicit; do not add open-ended language understanding as a silent contract.

## Needle PoC integration spec

This PoC must plug into the current Companion app structure. It may add one intent-resolution layer, but it must not add a second execution path.

### Integration point

- Keep `tools/abvx_companion.py` as the only Companion core for sync/import/export logic.
- Keep `tools/abvx_companion_app.py` as the only local UI/API host.
- Add one new UI/API layer for intent resolution, but route confirmed actions back into the same handlers already used by the current buttons/forms.
- Do not create a parallel worker, daemon, or background service for AI routing.

### API insertion

Add one new endpoint:

- `POST /api/intent/resolve`

Keep existing execution endpoints unchanged. The resolver endpoint only returns a structured proposal. It never starts work directly.

Current flow:

```text
UI action
  -> fixed API route
     -> Companion app handler
        -> Companion core operation
```

PoC flow:

```text
typed command
  -> /api/intent/resolve
     -> intent router
        -> confirmation card
           -> existing fixed API route
              -> Companion app handler
                 -> Companion core operation
```

### App-state contract

The Companion app should expose a small transient intent state:

```json
{
  "pending_intent": {
    "intent": "sync_music",
    "arguments": { "target": "sd" },
    "confidence": 0.93,
    "summary": "Sync prepared music mirror to the mounted SD card.",
    "requires_confirmation": true
  }
}
```

Rules:

- `pending_intent` is UI-only transient state.
- It is cleared on confirm, cancel, route failure, or page refresh.
- It is not persisted into backup state or sync logs.

### UI insertion

Add a small command entry field to the existing Companion UI:

- one text input,
- one resolve button,
- one result area for summary/fallback/reject,
- one confirmation card for mutating intents.

The command entry is an accelerator, not a replacement for the normal UI. Existing buttons and panels remain first-class.

### Handler mapping

Resolved intents must map onto existing Companion actions:

- `sd_status` -> existing status/load path
- `sync_time` -> existing time sync path
- `sync_music` -> existing music sync/import path
- `sync_books` -> existing books sync/import path
- `sync_voice` -> existing voice sync path
- `prepare_browser_package` -> existing fixed browser-package path when implemented

If an intent has no existing fixed handler, it is not eligible for the PoC.

### Preconditions contract

Before showing confirm, the app resolves current prerequisites from its own state:

- SD mounted when target is `sd`
- device/USB presence when target is `device`
- browser package feature enabled when requested
- voice content available when `sync_voice` is requested

If a prerequisite fails:

- do not mutate arguments,
- do not auto-run partial work,
- show fallback or actionable precondition failure in Companion terms.

### Logging contract

The app log should record:

- original command text,
- resolved intent,
- confidence,
- confirm/cancel outcome,
- final execution result if confirmed.

It should not record chain-of-thought or hidden model reasoning.

### Failure contract

- If the router is unavailable, hide or disable the command entry and keep the rest of the UI unchanged.
- If resolution fails, keep the user in the current panel and offer the ordinary workflow.
- If execution fails after confirmation, report it through the same status/progress surface used by current operations.

### Acceptance for integration

- One command can resolve into one Companion action without adding a new execution backend.
- Confirmed actions produce the same result as clicking the existing UI control.
- Disabling the router removes only the command-entry affordance, not any existing workflow.
- The integration does not weaken current host restrictions, token checks, or guarded flash/sync flows.

## Needle PoC file-level implementation map

This is the smallest acceptable implementation split for the first PoC.

### `tools/abvx_companion.py`

Add only bounded, reusable core helpers.

- Add one intent enum/source-of-truth list for the allowed PoC actions.
- Add one validator that accepts `intent + arguments` and returns normalized arguments or an error.
- Add one resolver-facing helper that exposes bounded preconditions in Companion terms, such as `sd_detected`, `usb_detected`, `voice_pending`, and `browser_package_enabled`.
- Do not add model/runtime code here.
- Do not add any new execution path here; core remains transport/conversion/sync logic only.

Expected additions:

- `INTENT_NAMES` or equivalent fixed tuple/set
- `intent_schema(intent)` or equivalent schema lookup
- `validate_intent_arguments(intent, arguments)`
- `intent_preconditions(sd_root, app_state)` or equivalent bounded state helper

### `tools/abvx_companion_app.py`

Add the PoC-specific API/UI glue here.

- Extend `AppState` with transient `pending_intent`.
- Add one helper to clear `pending_intent`.
- Add one helper to snapshot bounded router context from current app/device state.
- Add `POST /api/intent/resolve`.
- Add one small execution bridge that maps a confirmed intent back onto the existing fixed handlers.
- Keep existing `/api/...` execution routes as the only place where real work starts.

Expected additions:

- `AppState.pending_intent`
- `AppState.set_pending_intent(...)`
- `AppState.clear_pending_intent()`
- `intent_context_from_status(status)` or equivalent
- `resolve_intent(payload)` request handler
- `execute_confirmed_intent(intent, arguments)` mapping helper that internally reuses existing route logic

### Route mapping rule

The confirmed-intent bridge must map to current app behavior, not reimplement it.

- `sd_status` -> same status path used by the current UI
- `sync_time` -> same time-sync path already exposed by the app
- `sync_music` -> same music sync/import path already exposed by the app
- `sync_books` -> same books sync/import path already exposed by the app
- `sync_voice` -> same voice sync path already exposed by the app
- `prepare_browser_package` -> only when the app already has a fixed route for it

### UI surface

The current Companion HTML/UI should gain only a very small command surface.

- one text field
- one resolve button
- one result block
- one confirmation block
- one cancel action

No chat transcript, no streaming prose, no second panel model.

### Implementation order

1. Add core intent constants and argument validation in `abvx_companion.py`.
2. Add transient pending-intent state in `abvx_companion_app.py`.
3. Add `/api/intent/resolve` with fallback/reject semantics.
4. Add confirm/cancel UI wiring.
5. Add confirmed-intent bridge that reuses existing handlers.

### Done criteria

- A resolved `sync_music` request reaches the same operation as the current manual flow.
- A rejected or low-confidence request leaves all current Companion behavior unchanged.
- Disabling the PoC removes only the command-entry path and leaves the rest of the app intact.
## macOS runtime

`ABVx Companion.app` keeps its executable backend and UI inside the app bundle. On launch it installs a runtime copy under `~/Library/Application Support/ABVx Companion`; the firmware checkout remains the source used by Build and Flash. macOS may request removable-volume access when a physical SD card is used.
