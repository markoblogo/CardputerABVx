# ABVx Cardputer Mac Cockpit Design Contract

## Design read

Reading this as a local Mac operational cockpit for one Cardputer and its bounded companion services. It should be glanceable like a device console: identity, connection, freshness, logs, safe actions, and proof before decoration.

## Scope and source ownership

This contract applies to the Mac Companion UI whose source is `tools/companion_ui/index.html`. The packaged app/runtime copies are delivery artifacts; do not edit them as the design source unless the packaging workflow explicitly requires synchronization. Firmware display constraints remain separate.

## Reference direction

Use [bitdrift live logs](https://bitdrift.io/use-cases/live-streaming) as a pattern reference: device logs and freshness are primary, with source identity and filters close to the stream. Extract hierarchy and operational legibility; do not clone its marketing page or terminal styling.

## Dials

- Variance: low. Controls and state must stay predictable.
- Motion: near zero. Device work should respond immediately.
- Density: high, but grouped by device state, operation, and risk.

## Preserve

- Current dark green ABVx terminal identity where contrast remains sufficient.
- Local-only network boundary, explicit paths, and visible service/device identity.
- `AUTO FLASH: NEVER`, confirmation for flashing, and bounded command routing.
- Separation between read-only local-model answers and device-control operations.
- SD, USB, ESP-IDF, firmware, import, queue, sync, and Activity states.
- Existing file contracts and honest hardware/software proof boundaries.
- The Companion remains fully usable when optional AI/runtime helpers are unavailable.

## Reconsider

- A grid of equal cards that makes status, routine imports, AI, errors, and firmware look equally important.
- Activity logs placed after all controls instead of acting as the operational record.
- Status repeated across rail, hero, cards, queue, and toast without one canonical summary.
- Dangerous firmware operations visually adjacent to routine content import.
- Animation on status dots, drop targets, or section highlights when text state is enough.
- AI input competing with device identity and storage/connection state.

## Primary information hierarchy

1. Workspace identity: expected Cardputer, mounted SD path, USB target, and Companion backend.
2. Current state: connected/offline, last refresh, storage, firmware/build identity, and AI worker availability.
3. Activity stream: timestamped operation, target, phase, result, and recovery hint.
4. Pending queue and explicit confirmation/cancellation boundary.
5. Routine operations: sync, import, export, time, and status refresh.
6. Optional AI question/context surface.
7. Firmware build/flash in a clearly separated high-risk region.

Every operation must expose its target and final state. Build proof, flash proof, mounted-volume proof, and on-device behavior are separate claims.

## Layout and components

- Use a persistent top status strip with device/backend identity and freshness.
- Make Activity a large, continuously readable primary pane rather than a footer card.
- Pair the queue with Activity so pending, running, completed, failed, and cancelled work form one chronology.
- Group routine operations by intent; do not create one card per minor function.
- Put destructive firmware actions in a separated zone with target identity and typed/explicit confirmation where risk warrants it.
- Show AI worker status, model/device, explicit context scope, and `read_only` authority next to every answer.
- Logs use tabular timestamps, severity labels, wrapping for messages, and controlled horizontal scrolling for code/paths.

## Color and type

- Keep the current near-black/green palette as product identity, not as a reason to color all text green.
- Primary text remains neutral and high contrast; acid green marks active/healthy focus only.
- Yellow means attention, red means failure/destructive, and both require labels.
- Monospace is appropriate for logs, paths, ports, IDs, and status values; prose and instructions may use a more readable UI sans stack.

## Motion

- No motion for keyboard actions, queue navigation, logs, or repeated sync/import controls.
- Replace perpetual heartbeat/pulse effects with timestamp and explicit `LIVE`, `STALE`, `OFFLINE`, or `UNKNOWN` text where possible.
- Short progress motion is allowed only while work is genuinely running.
- All state remains clear under `prefers-reduced-motion`.

## Responsive behavior

- Desktop Mac is primary; the cockpit must remain usable in a narrow app window.
- Preserve top status, Activity, operation target, and dangerous-action boundaries before secondary guidance.
- Long paths and logs scroll or wrap intentionally; they may not widen the entire page.
- Touch-sized controls are desirable, but keyboard focus and shortcuts must stay first-class.

## Anti-patterns

- Fake “live” LEDs without freshness timestamps.
- Green terminal cosplay that reduces readability.
- Automatic flash, hidden target selection, or confirmation inferred from an AI answer.
- Treating a local model response as device state or hardware proof.
- Toast-only success/failure with no durable Activity record.
- Animating every device-state update.

## Verification gate

Test no-SD, mounted-SD, no-USB, detected-USB, worker-offline, worker-ready, queue-running, operation-failed, import-success, and flash-confirmation states. Verify keyboard/focus, narrow window, long paths/logs, reduced motion, explicit target identity, and that AI cannot trigger device control. Hardware behavior still requires a real-device smoke test.

## First redesign surface

Start with the single Companion cockpit viewport: top identity/status strip, primary Activity + queue region, routine operation rail, and separated firmware/AI regions. Preserve all existing operations and authority boundaries; this is an information-architecture redesign before a style rewrite.

## Implemented slice

The first Companion pass promotes Activity, the operation queue, and export freshness immediately below device identity. Routine content operations, optional local AI guidance, onboarding, firmware, and safety remain available in descending operational priority. Existing element IDs, commands, confirmation gates, and local-only authority are preserved.
