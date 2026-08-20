# Mac Companion References

Future ABVx Mac Companion should be a small local utility for preparing and syncing Cardputer SD content. It is not part of the firmware MVP.

## Target Mac Companion scope

- Books: import EPUB/FB2/PDF/DOCX/TXT, convert to clean TXT, split/normalize if needed, copy to `/books`.
- Music: import common audio formats, convert to Cardputer-friendly MP3, enforce safe filenames, copy to `/music`.
- Notes: pull/edit/export `/notes`, push plain TXT back.
- Recordings: pull `/rec` and `/RECS`, preview/export WAV.
- Device: SD status, time sync, config editing, firmware update later.

Initial transport should be SD reader / mounted volume. Wi-Fi transfer can be added after firmware transfer is stable.

## BookOrbit reference

Repository: https://github.com/bookorbit/bookorbit

BookOrbit is a self-hosted digital library platform for ebooks, PDFs, audiobooks, comics, metadata, OPDS, Kobo/KOReader sync, and web reading.

Useful as reference for ABVx:

- Library model: books, audiobooks, metadata, reading state.
- Import/drop-folder workflow: stage files, normalize, then finalize into a library.
- Multi-format thinking: EPUB/MOBI/AZW3/PDF/audio handling.
- Metadata enrichment concepts.
- OPDS / KOReader ideas for long-term reader ecosystem compatibility.
- UX pattern: user drops messy source files; app prepares device-ready output.

Not recommended as direct base for ABVx Mac Companion:

- It is a large server/web platform, not a lightweight Mac companion.
- Stack and runtime are much heavier than needed for local SD preparation.
- It solves multi-user/self-hosted library management, while ABVx needs single-user file preparation and device sync.
- It is licensed AGPL-3.0, so direct code reuse requires careful license compliance.

Decision:

- Use BookOrbit as product/architecture reference only.
- Do not copy code into ABVx without explicit licensing review.
- Build ABVx Mac Companion as a small focused app first.

## Needle reference

Site: https://cactuscompute.com/needle

Potential value for ABVx:

- Very small on-device tool-calling model shape.
- Strong focus on structured extraction and fixed-schema function routing.
- Confidence-based local fallback model fits Companion confirmation flows.
- Better donor candidate for command routing than for general chat.

What to adopt:

- The idea of a tiny local router over fixed Companion tools.
- Confidence-gated execution with ordinary UI fallback.
- Structured intent extraction for short commands such as sync, export, and status.

What not to adopt:

- Do not treat it as approval for firmware-side AI in the active roadmap.
- Do not add open-ended offline chat to Companion.
- Do not bypass existing conversion/validation/deploy layers with model-driven actions.
- Do not import a donor runtime until there is a bounded PoC and clear packaging/runtime proof on the target Mac flow.

Minimum acceptable PoC:

- Mac Companion only.
- 5-8 fixed tool schemas.
- Read-only commands can run directly; mutating commands require confirmation.
- Output must be structured intent, confidence, and a human-readable summary.
- Failure path is simple: fall back to the existing buttons/forms.

Acceptance criteria:

- It saves time on real Companion tasks instead of adding a second UI.
- It never guesses silently on destructive or ambiguous requests.
- It does not change source->mirror->SD invariants.
- Removing it leaves the Companion fully usable.

## llmfit reference

Repository: https://github.com/AlexsJones/llmfit

Potential value for ABVx:

- Planning-only donor for host-side model selection on the Mac.
- Useful for shortlisting small local models by hardware fit before any Companion AI experiment.
- Helpful as a benchmark/planning layer for comparing candidate routing models on the actual developer machine.

What to adopt:

- Use it only to narrow the candidate set for future Companion-side routing experiments.
- Prefer it for Mac-host hardware planning, not for product/runtime integration.

What not to adopt:

- Do not add it to Cardputer firmware.
- Do not treat it as a runtime dependency of `ABVx Companion`.
- Do not let it widen the product scope into general local-LLM hosting.

Decision:

- Approved only as a planning/benchmark donor for host-side model selection.
- Not approved as a user-facing or firmware-facing dependency.
