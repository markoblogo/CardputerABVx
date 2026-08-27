# ABVx Adventure / GPS / LoRa Architecture

Status: GNSS Lab, Journey CSV, and Running Mode firmware slices are implemented. Journey-to-Music navigation and background playback, including automatic track changes, are hardware-verified. GNSS hardware verification is blocked because the connected Cap currently reports `UART B0 L0`; outdoor fix and coordinate logging remain unaccepted. Voice Marks, GPX export, and LoRa remain separate later phases.

Journey tracking is a background session: it may continue while Music is open or playing. GNSS polling remains active and CSV points are bounded to one write every 30 seconds. CSV writes pause on Reader and Recorder screens, which retain their existing SD-safety constraints.

The Journey screen requires explicit `OK STOP`; `GO/BACK` shows `STOP FIRST`. `M` opens Music without ending the Journey, and `J` returns from Music without stopping playback. Automatic track changes preserve Journey/Running instead of forcing the Music screen. The first Running Mode presents distance, estimated pace from recorded points, and elapsed session time; it is deliberately a local-only MVP, not a precision sports watch claim.

GNSS Lab exposes raw UART bytes and lines separately from checksum-valid NMEA. `UART B0` means no electrical UART traffic and must be resolved as a power/contact/module issue before evaluating sky visibility or time-to-first-fix.

## Hardware target

Planned module: M5Stack Cap LoRa 1262 for Cardputer ADV.

Relevant hardware capabilities:

- LoRa: SX1262.
- Frequency range: 868-923 MHz.
- Interface: SPI.
- TX power: up to +22 dBm.
- Receiver sensitivity: down to -147 dBm in low-data-rate LoRa mode.
- GNSS: ATGM336H/AT6668 class module.
- GNSS interface: UART, NMEA 0183.
- GNSS systems: GPS, QZSS, BeiDou, Galileo, GLONASS.
- GNSS update rate: up to 10 Hz.

Sources:

- M5Stack product page: https://shop.m5stack.com/products/cap-lora-1262-for-cardputer-adv-sx1262-atgm336h
- EU868 duty-cycle reference: https://www.thethingsnetwork.org/docs/lorawan/regional-limitations-of-rf-use/

## Product framing

This module should not turn ABVx into a generic radio terminal.

The valuable product direction is:

```text
Pocket OS -> Adventure Mode -> local-first journey memory
```

There are exactly two intended jobs:

1. GPS/GNSS context for outdoor activity and journey memory.
2. Direct low-bandwidth data exchange with a computer/home station that has another LoRa module.

LoRa is not planned as mesh networking and not planned as Meshtastic integration.

## Key constraints

### LoRa is not file sync

LoRa must be treated as a low-bandwidth, low-duty-cycle channel.

Good payloads:

- GPS point;
- workout status;
- checkpoint;
- short text note;
- habit/event log;
- short command;
- time sync;
- small config;
- emergency/status message.

Bad payloads:

- MP3;
- books;
- recordings;
- images;
- large sync;
- continuous telemetry spam.

### EU duty-cycle limits matter

EU 868 MHz operation can be limited to 0.1%, 1%, or 10% duty cycle depending on exact sub-band. ABVx should default to conservative short packets and local buffering.

Design rule:

```text
store locally first
send short packets opportunistically
retry later if no relay
```

## Feature evaluation

### Breadcrumbs

Keep.

Use GPS to store route points during walk/run/ride.

MVP:

```text
/sdcard/journeys/J0001/TRACK.CSV
```

Fields:

```text
ms,lat,lon,alt,speed,fix,sats
```

Later export GPX in Mac Companion.

### Running Mode

Keep high priority after GNSS bring-up.

Screen should be purpose-built, not app-menu style:

```text
07.32 km
5:18 /km
38:12
```

Controls:

- OK: lap / mark;
- R: voice mark;
- GO: stop/end;
- Up/Down: music volume if music is playing.

### Voice Marks

Keep. Strong ABVx feature.

A voice mark is:

```text
GPS + time + short recording
```

Storage:

```text
/sdcard/journeys/J0001/MARK0001.WAV
/sdcard/journeys/J0001/MARKS.CSV
```

### Running Inbox

Keep.

This is One Button Capture with GPS context:

```text
R -> record 5-10 sec -> attach current GPS -> save into current journey
```

### Trail Memory / Places

Keep as later feature.

Do not implement first. It needs reliable local place matching and history.

MVP later:

```text
/sdcard/places/PLACES.TXT
```

Fields:

```text
id,name,lat,lon,radius_m,last_visit,last_context
```

### Bicycle / Ride Mode

Keep, but second after Running Mode.

It reuses the same Journey engine:

- distance;
- speed;
- moving time;
- elevation from GPS;
- music controls;
- voice marks.

### Adventure Mode

Keep as umbrella feature.

Adventure Mode is a session container:

```text
Journey
├─ GPS track
├─ voice marks
├─ text notes
├─ music listened
├─ reader context
└─ summary
```

Storage:

```text
/sdcard/journeys/J0001/META.TXT
/sdcard/journeys/J0001/TRACK.CSV
/sdcard/journeys/J0001/MARKS.CSV
/sdcard/journeys/J0001/*.WAV
```

### Places

Keep later.

Places makes the device feel alive:

```text
Welcome back
Last time: 18 May
Reading: Meditations p117
```

Implement only after Journey history exists.

### Direct Computer Relay

Keep after GPS/Journey MVP. This is a direct Cardputer-to-computer/home-station link using another LoRa module connected to the computer/server.

Architecture:

```text
Cardputer
  local journey/event buffer
  LoRa short packet
Computer LoRa station
  ACK / TIME_SYNC / MESSAGE
Mac/NAS
  inbox / timeline / GPX builder
```

Packet classes:

Cardputer -> Home:

- PING;
- STATUS;
- GPS_POINT;
- MARK;
- NOTE_SHORT;
- WORKOUT_SUMMARY;
- LOW_BATTERY;
- RETURNED_HOME.

Computer relay -> Cardputer:

- ACK;
- TIME_SYNC;
- MESSAGE;
- TODO;
- CONFIG_SMALL.

Important: relay failure must never block local recording or journey tracking.

## Recommended implementation order

### Phase A: GNSS Lab

Goal: prove hardware access.

- Detect module.
- Read NMEA over UART.
- Show fix/no-fix, satellites, lat/lon, speed, altitude.
- No SD writes at first.

Acceptance:

- Cardputer shows live coordinates outdoors.
- App remains stable without fix.

### Phase B: Journey / Breadcrumb MVP

Goal: local GPS track.

- Start/stop Journey.
- Log points every 30-60 seconds or every 100-200 m.
- Store CSV on SD.
- Show distance/time/speed.

Acceptance:

- A walk/run creates one journey folder and a readable track file.

### Phase C: Running Mode MVP

Goal: sports-computer screen.

- Large distance/pace/time screen.
- Music can continue.
- OK lap/mark.
- GO end.

Acceptance:

- Running Mode works without phone and without LoRa relay.

### Phase D: Voice Marks

Goal: attach voice notes to GPS.

- R records short mark.
- Save WAV + GPS/time row in MARKS.CSV.

Acceptance:

- User can record a mark during a walk/run and find it in Journey folder.

### Phase E: Mac Companion GPX builder

Goal: process Cardputer journeys on Mac.

- Import TRACK.CSV.
- Export GPX.
- Show voice marks on map later.

Acceptance:

- A Cardputer run can become a GPX file.

### Phase F: LoRa Lab

Goal: prove radio, not product feature yet.

- PING between Cardputer and home relay.
- RSSI/SNR display.
- Duty-cycle-safe send interval.

Acceptance:

- Cardputer can send and receive short packets reliably.

### Phase G: Direct Computer Relay MVP

Goal: sparse event exchange with a computer/home station connected to another LoRa module.

- Send STATUS and WORKOUT_SUMMARY.
- Relay ACKs.
- Relay can send TIME_SYNC.

Acceptance:

- No file sync over LoRa.
- Missed relay does not lose local data.

## What not to do first

Do not start with:

- full LoRa mesh;
- Meshtastic integration;
- generic LoRa chat/networking as a product goal;
- continuous live tracking every second;
- voice/audio over LoRa;
- map rendering on Cardputer;
- GPX generation on Cardputer;
- complex Places matching before Journey logs exist.

## Product priority

Highest value path:

```text
GNSS Lab -> Journey Track -> Running Mode -> Voice Marks -> Mac GPX export -> Direct Computer LoRa Relay
```

This preserves the offline-first product philosophy and avoids using LoRa for the wrong job.
