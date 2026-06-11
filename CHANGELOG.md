# Changelog

All notable changes to this project will be documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [1.1.0] — 2026-06-11

### Added
- CAN-native fault state decoded: `0x2C4 D1=0x0B` confirmed as active fault state via two controlled captures
- D3 fault code byte confirmed: `0x20` = flame loss / fuel starvation, `0x40` = overtemperature / exhaust blocked
- ESPHome component (`espar_can`) updated to detect and name CAN fault codes in real time
- Arduino tester sketch (`ESPAR_CAN_Tester_WeAct.ino`) — full controller sketch for the WeAct CAN485 DevBoard V1 with fault detection, auto-idle on fault, `f` command for fault status query
- `ESPAR_CAN_Fault_Analysis.md` — full narrative analysis of both fault captures with frame-by-frame decode
- `ESPAR_CAN_Signal_Map.md` — updated signal map with fault state, D3 fault codes, D2 sub-states, and fault path in state diagram

### Changed
- `espar_can.h` / `espar_can.cpp` — fault detection updated from behavioral-only to CAN-native: reads D1=0x0B and D3 fault code, publishes named fault string (`FAULT:FLAME_LOSS`, `FAULT:OVERTEMP`)
- `docs/fault-codes.md` — updated status from "behavioral detection only" to reflect decoded fault state; Phase 2 section now documents confirmed D3 values; capture plan refocused on remaining unknown D3 bits
- `captures/fault-codes/README.md` — updated from empty placeholder to reflect two analyzed captures and remaining capture targets

### Fixed
- `arduino/ESPAR_CAN_Tester_WeAct.ino` — corrected bus speed from 250 kbps to 500 kbps to match confirmed protocol

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
- Remaining D3 fault code bits (bits 0–4, bit 7) not yet captured
- Only tested on Airtronic S3 B2L Gasoline 12V
