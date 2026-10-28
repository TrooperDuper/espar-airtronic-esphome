# Changelog

All notable changes to this project will be documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

### Fixed
- **OTA update while the heater runs** ([#27](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/27)). The upload blocks the main loop for ~20–30 s; a busy heater that loses its controller for more than ~15 s ends the run by itself and stores P000307 + P000440 (measured by [@farnham26](https://github.com/farnham26) on a Hydronic S3). Now, when an OTA starts with the heater busy, the component commands IDLE (a normal stop with cooldown) and a small task keeps sending the heartbeat and the IDLE command group until the reboot. The climate entity comes back as Off. New option `ota_safe_stop` (default `true`, needs an `ota:` block); nothing changes when the heater is idle or in CAN standby, or when another controller is on the bus.

---

## [2.0.0] — 2026-10-28

The heater now regulates itself, as with the OEM EasyStart Pro, and the component can read the heater's own diagnostics. Several config options were removed — see **Breaking** below and *Upgrading* in `esphome/README.md`.

### ⚠️ Breaking
- **The heater regulates itself** ([#9](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/9), [#19](https://github.com/TrooperDuper/espar-airtronic-esphome/pull/19)). In HEAT the climate target goes to the heater as its setpoint, together with a real room temperature in 0x54 D5/D6 ((°C + 50) × 10). The heater starts, modulates, pauses and restarts by itself — no more short-cycling (full cooldown and new glow-plug ignition at every setpoint crossing). The climate action shows *Idle* during the heater's control pause (FD10 phase `0x28`), where 0x2C4 still reports HEATING.
- **Removed options:** the component's own thermostat with `heat_setpoint_f` and `pause_mode`. A config that still sets either fails validation with a message saying so — delete the option. (`hysteresis`, `min_run_time`, `min_off_time` and `max_starts_per_hour` existed briefly on `main` and are rejected the same way.) The heat-confirm timeout is gone too: in its control pause the heater rightly doesn't heat.
- **Entity rename:** the example config's `flame_active` entity is now **"Espar Heating"** (was "Espar Flame Active"); it follows the heater's HEATING state (0x2C4 D1=0x09), ignition ramp included. Home Assistant creates `binary_sensor.espar_heating` and leaves the old entity orphaned — see **Upgrading** in `esphome/README.md`. The YAML key `flame_active` is unchanged.
- **Don't filter the cabin sensor:** remove `filter_out: nan` from the `current_temperature_sensor` — it hides *unavailable*, which the new fallback needs.

### Added
- **`room_sensor`** — what the heater regulates on: `heater` (default; its own intake sensor read over UDS, minus its 3.0 K correction) or `controller` (`current_temperature_sensor`). Switchable at run time with `set_room_sensor_controller()`.
- **Cabin-temperature fallback:** with `room_sensor: controller`, the heater's own sensor takes over while the cabin value is unavailable, or stale (no update *and* no HA connection for 5 min). *Espar Fault* shows `CABIN TEMP LOST — using the heater's own sensor` until it's back.
- **`max_run_time`** (default `120min`, `0min` = no limit): HEAT or FAN ONLY switch off after this time, however they were started — the heater has no limit of its own. Changeable at run time with `set_max_run_time_minutes()`.
- **Optional read-only diagnostics over UDS** (0x7A0 → 0x73C), a new `diagnostics:` block: real flame detection, combustion phase, flame and intake temperature, supply voltage, fuel pump, glow plug, fuel rate and lifetime fuel, power demand, setpoint/regulating temperature, air pressure, the heater's operating-hour counters, versions/part number and the **fault memory with P-codes**. Only ReadDataByIdentifier and ReadDTCInformation are sent — nothing is written, reset or cleared, and nothing is sent in CAN Standby. By [@rhd944dot1](https://github.com/rhd944dot1), building on [@farnham26](https://github.com/farnham26)'s Hydronic research.
- **`esphome/espar-heater-full.yaml`:** a second example with every diagnostic entity, a room-sensor select, *Max Run Time* as a number entity and a grouped web dashboard. `espar-heater.yaml` stays the minimal example.
- **COOLDOWN heater state:** the ~4 min after-run (D1=IDLE with D2 bit 3, or the one-frame `0x01` transition) shows as `COOLDOWN`, climate action *Fan*. The example YAML adds a dim-orange LED colour.
- **IDLE-lock release** ([#14](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/14)): after a power interruption the heater can sit in IDLE with D3 = `0x04` and ignore commands. The component reports it (*Espar Fault* = `IDLE LOCK: …`) and releases it automatically (`auto_clear_idle_lock`, default on).
- **Other controller on the bus:** another controller's 0x60D or 0x54–0x57 frames while the WeAct transmits set *Espar Fault* = `OTHER CONTROLLER ON BUS — enable CAN Standby` (re-armed when Standby is turned off).
- `controller_id` and `init_0x62_byte` options (0x60D controller ID and last byte of init frame 0x62). Defaults unchanged.
- Optional diagnostic binary sensor `flag_0x20` (0x2C4 D2 bit 5 — altitude compensation active).

### Changed
- Climate entity minimum setpoint lowered from 15 °C (59 °F) to 10 °C (50 °F), the heater's own lower limit (Airtronic 3 Technical Description §4.4).
- CAN bus errors are logged as one summary every 5 s instead of one line per error (the WeAct retries endlessly while the heater is unpowered).
- 0x2C4 status counts as heater liveness, not only the 0x625 heartbeat. A real Pro's frames (0x54–0x57, 0x60D, 0x65, init burst) and diagnostic traffic (0x7A0/0x73C) are no longer raised as unknown-ID faults.
- Every change of the 0x2C4 D3 fault byte is logged at WARN; an unmapped heater state logs its raw D1–D4 bytes.
- Status LED migrated from deprecated `neopixelbus` to `esp32_rmt_led_strip`; LED colour actions set `effect: "None"` so the fault pulse stops once the fault clears.

### Fixed
- Heater ignored commands after a power interruption while reporting OK ([#14](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/14)) — see IDLE-lock release above.
- Heartbeat-timeout underflow that caused connect/disconnect flapping.
- ESPHome 2026.5+ compatibility (ClimateTraits feature flags).

### Documentation
- **UDS diagnostics** (`docs/uds-diagnostics.md`) and the **heater-regulation experiment** (`docs/experiments/heater-regulation.md`) by [@rhd944dot1](https://github.com/rhd944dot1).
- **0x2C4 / 0x54 D5–D6 = room temperature**, (°C + 50) × 10, confirmed over UDS (FD45). The init burst carries the controller's settings (sensor selection, offset, timers, clock), not an address.
- **Flag 0x20 = altitude compensation:** 793 hPa and 286 h in altitude mode on a B2L at ~2,100 m; 0 h and never set at sea level. It comes on during pre-glow, before flame detection.
- **P000342 = "invalid configuration: too many CAN units"** (Eberspächer Hydronic S3 repair instructions, via [@farnham26](https://github.com/farnham26)). ADR is the dangerous-goods mode, not a bus address (thanks Greg, [@gregoryxxxxxx](https://github.com/gregoryxxxxxx)). One controller at a time; the Remote+ and Select use LIN, not CAN ([#15](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/15)).
- After a run the heater's own (intake) sensor reads ~15 K high for 30+ min from residual heat.
- D3 = `0x04` (power-up lock) in the fault tables; controller-identity test results in `docs/experiments/controller-id.md`; Acknowledgments section in the README.

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
