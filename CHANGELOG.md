# Changelog

All notable changes to this project will be documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [1.0.0] — 2026-05-06

Initial public release.

### Added
- ESPHome external component (`espar_can`) with climate entity (HEAT / FAN_ONLY / OFF)
- Thermostat logic using averaged external cabin sensor vs. HA setpoint
- Fixed high setpoint (configurable, default 85°F) sent to heater hardware
- Behavioral fault detection: heartbeat loss, failed start, startup timeout, heat confirm timeout, lockout after 10 consecutive failed starts
- WS2812 RGB status LED with color codes (amber=heating, blue=fan, dim green=idle, red pulse=fault)
- TWAI bus-off recovery — automatically initiates recovery when ESP32 CAN controller goes bus-off
- Unconditional RX queue drain — frames no longer dropped when alert bit is consumed by error interrupt
- CAN heartbeat timeout bumped to 10s to tolerate occasional >5s gaps in heater's 0x625 cadence
- Arduino sketches: full controller (`CAN_CSV_WeAct.ino`) and passive listener (`CAN_CSV_WeAct_Listen.ino`)
- `tools/decode.py` — SavvyCAN format converter and frame annotator
- 19 CAN captures across all RE phases (Phase 1A, 1B, Phase 2A–2J)
- EasyStart Pro 71k-frame session split into 8 × ~475 KB parts
- Full protocol reference, hardware setup guide, and fault code table (Eberspächer service manual P-codes)

### Protocol confirmed
- 0x54 HEAT at 66°F, 75°F, 78°F, 80°F (4 confirmed setpoints; 78°F proves 16-bit LE encoding)
- 0x54 FAN ONLY (D2=0x02, confirmed from capture)
- 0x2C4 state byte: 0x02=STARTUP, 0x03=IDLE, 0x09=HEATING, 0x21=FAN
- 0x625 heater heartbeat structure
- Full init burst sequence (0x5C–0x10A)

### Known gaps
- Fault frame CAN IDs not yet decoded
- Only tested on Airtronic S3 B2L Gasoline 12V
