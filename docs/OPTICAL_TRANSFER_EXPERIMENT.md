# Optical Transfer Experiment

Cardputer can treat screen-to-camera optical transfer as a future offline
transport experiment for small payloads.

The donor pattern is `bashalarmistalt/decimen-optical-transfer`: a sender shows
an endless stream of animated QR frames and a receiver reconstructs a payload
from enough decoded frames. This note records the local Cardputer adaptation
boundary only. No code is imported here.

Source: https://github.com/bashalarmistalt/decimen-optical-transfer

## Candidate scenarios

- transfer small payloads without Wi-Fi, Bluetooth, or USB;
- send configs, logs, pairing tokens, recovery packets, or firmware metadata;
- Mac Companion or phone shows QR stream, Cardputer receives through camera or
  camera module;
- Cardputer shows tiny QR output to phone or Mac for very small diagnostics;
- emergency/offline handoff for signed command or diagnostic bundles;
- BrowserApp troubleshooting page links to optical import/export as a future
  experiment.

## First pilot direction

Start with:

```text
Mac or phone screen -> Cardputer receiver
```

Do not start with Cardputer as sender for large payloads. The display is small
and better suited to tiny diagnostics or manual confirmation codes.

## Payload limits

Initial experiment should use:

- tiny JSON payloads first;
- explicit maximum byte cap;
- manifest before data use;
- checksum before persistence;
- signature before any command-like effect;
- no raw secrets unless encrypted outside the transfer layer.

## Receiver guardrails

The receiver must reject:

- inconsistent `total_len`;
- payloads larger than the configured cap;
- unknown payload type;
- missing checksum;
- failed checksum;
- expired command packet;
- unsigned command packet;
- transfers that exceed timeout.

## Open hardware checks

Before implementation, confirm:

- camera or camera-module path;
- QR decode library that fits memory and build constraints;
- achievable camera FPS;
- decode latency on device;
- safe heap allocation for partial chunks;
- screen brightness and distance requirements;
- fallback when frames are missed.

## Current status

`reference_only`.

Promote to `pilot_candidate` only after hardware decode path is confirmed.
