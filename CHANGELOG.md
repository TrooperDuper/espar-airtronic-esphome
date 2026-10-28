# Changelog

All notable changes to this project will be documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased]

### ⚠️ Breaking — the heater regulates (#9)
- **The heater regulates itself, always:** in HEAT the climate target goes to the heater as its setpoint together with a real room temperature in 0x54 D5/D6 ((°C + 50) × 10), and the heater starts, modulates, pauses and restarts by itself, like with the OEM EasyStart Pro. The component no longer switches the heater on and off, so there is no short-cycling (no full cooldown and new glow-plug ignition at every setpoint crossing). Tested on an Airtronic S3 B2L, incl. a 55 min run (docs/experiments/heater-regulation.md).
- **Removed:** the component's own thermostat and its options `heat_setpoint_f`, `pause_mode`, `hysteresis`, `min_run_time`, `min_off_time`, `max_starts_per_hour` (the last four were added after #14 and never released). A config that still sets one of them fails validation with a message saying so — delete the option. The heat-confirm timeout is gone too: in its control pause the heater rightly doesn't heat.
- **`room_sensor`**: `heater` (default — the heater's own intake sensor, read over UDS, minus its 3.0 K correction; works without a `diagnostics:` block) or `controller` (`current_temperature_sensor`, the former thermostat input). Switchable at run time with `set_room_sensor_controller()`; a change re-sends the settings frames with the 0x63 change counter, as the OEM controller does.
- **Lost cabin temperature:** with `room_sensor: controller` the heater's own sensor takes over while `current_temperature_sensor` has no value (HA entity unavailable) or is stale — no update and no HA connection for 5 min (an HA sensor only sends changes, so a steady value counts as current while HA is connected). Logged at WARN, *Espar Fault* = `CABIN TEMP LOST — using the heater's own sensor`, cleared when the value is back. The example configs no longer put `filter_out: nan` on the cabin sensor — it hid *unavailable* from the component.
- **`max_run_time`** (default `120min`, `0min` = no limit): HEAT or FAN ONLY is switched off after this time, however it was started — the heater has no limit of its own. Part of the component, so every config has it; changeable at run time with `set_max_run_time_minutes()`.
- **`esphome/espar-heater-full.yaml`**: a second example config with a select to switch the room sensor at run time, every diagnostic entity, *Max Run Time* as a number entity (sets `max_run_time` at run time, 0 = no limit) and the web dashboard sorted into groups. `espar-heater.yaml` stays the minimal example.
- The climate action shows *Idle* during the heater's control pause (FD10 phase `0x28`), where 0x2C4 still reports HEATING.

### Added — heater diagnostics
- **Optional, read-only diagnostics over UDS** (0x7A0 → 0x73C): a new `diagnostics:` block on `espar_can`. Off unless the block is present. Only ReadDataByIdentifier and ReadDTCInformation (`19 02 08`) are sent; nothing is written, reset or cleared, and nothing is sent in CAN Standby or while the heater isn't connected. Entities include real flame detection, combustion phase, flame and intake temperature, supply voltage, fuel pump frequency, glow plug, fuel rate and lifetime fuel, the heater's operating-hour counters, versions/part number and the **fault memory with P-codes** (read on connect, on a fault, after each run, or via `read_fault_memory()`). Polled every `update_interval` (default 10 s) while the heater is active, once a minute while idle. See *Heater diagnostics* in `esphome/README.md` and `docs/uds-diagnostics.md`.
- Diagnostic answers on 0x73C and another tester's requests on 0x7A0 are no longer reported as unknown IDs.

### Added (after #14)
- **COOLDOWN heater state:** D1=IDLE with D2 bit 3 and no fault code = after-run blower. *Espar Heater State* shows `COOLDOWN` and the climate action shows *Fan* until it's truly idle. The example YAML adds a dim-orange LED colour.
- **Other controller on the bus:** another controller's 0x60D or 0x54–0x57 frames while the WeAct is transmitting set *Espar Fault* = `OTHER CONTROLLER ON BUS — enable CAN Standby` (cleared when it disappears or Standby is turned on). 0x60D, 0x65 and the init-burst IDs are now treated like 0x54–0x57: logged at debug, not raised as unknown-ID faults.
- Other-controller detection is re-armed when CAN Standby is turned off, so a Pro left on the bus raises the fault instead of being silently ignored.
- An unmapped 0x2C4 state (shown as `UNKNOWN`) now logs its raw D1–D4 bytes at WARN, so it can be identified.

### Changed (after #14)
- CAN bus errors are logged as one summary every 5 s instead of one line per error. With the heater unpowered nothing ACKs the WeAct's frames and the error count climbs by thousands per second, which previously stalled the main loop.
- Climate entity minimum setpoint lowered from 15 °C (59 °F) to 10 °C (50 °F), matching the heater's own setpoint range (Airtronic 3 Technical Description §4.4). Override with the standard `visual:` block if needed.

### Fixed
- **Heater ignored commands after a power interruption while reporting OK** ([#14](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/14)). The heater often comes back in IDLE with D3 = `0x04` and ignores HEAT/FAN until 0x54 goes FAN → IDLE. The component now reports it (*Espar Fault* = `IDLE LOCK: …`) and releases it automatically (`auto_clear_idle_lock`, default on: 45 s wait, FAN 6 s, IDLE 5 s, up to 3 tries). Verified 2/2 on hardware.
- Heartbeat-timeout underflow that caused connect/disconnect flapping (sample `now` after RX).
- ESPHome 2026.5+ compatibility: ClimateTraits feature flags.

### Added
- `controller_id` and `init_0x62_byte` options (0x60D controller ID and last byte of init frame 0x62). Defaults unchanged. The heater accepted every value tested.
- Every change of the 0x2C4 D3 fault byte is logged at WARN.
- Example config: status LED colour actions set `effect: "None"`, so the red fault pulse stops once the fault clears.
- 0x2C4 status also counts as heater liveness (not only the 0x625 heartbeat). A real EasyStart Pro's 0x54–0x57 frames are ignored instead of raised as unknown-ID faults.

### Changed
- `flame_active` now follows the heater's HEATING state (0x2C4 D1=0x09) instead of D2 bit5. The raw D2 bit5 is exposed as a new optional diagnostic binary sensor `flag_0x20` (meaning unconfirmed; absent on a sea-level B2L — leading theory is altitude compensation). The YAML key `flame_active` is unchanged.

### ⚠️ Breaking — entity rename
- The example config renames the `flame_active` entity from **"Espar Flame Active"** to **"Espar Heating"** (it's on during the ignition ramp too, so "flame" overstated it). In Home Assistant this creates a new entity (`binary_sensor.espar_heating`) and leaves the old `binary_sensor.espar_flame_active` behind as an unavailable, orphaned entity. Delete it and update any automations/dashboards that used it. See **Upgrading** in `esphome/README.md` for step-by-step cleanup, including how to keep the old entity ID.
- P000342 corrected to "invalid configuration — check ADR coding" (not "too many CAN controllers"). ADR is the European dangerous-goods agreement (Eberspächer's ADR mode for hazardous-cargo vehicles), not a bus address; the earlier "ADR address" reading was wrong. Thanks to Greg ([@gregoryxxxxxx](https://github.com/gregoryxxxxxx)) for spotting it. The WeAct + Pro conflict is likely both sending the same CAN IDs.
- Status LED migrated from deprecated `neopixelbus` to `esp32_rmt_led_strip`.

### Documentation
- **P000342 is "invalid configuration: too many CAN units"**, per Eberspächer's Hydronic S3 repair instructions (via [@farnham26](https://github.com/farnham26)). This refines the P000342 entry under Changed above: ADR is the dangerous-goods mode, not a bus address, and the fault means more than one control unit on the bus. One controller at a time (on a Hydronic, the EasyStart answered another controller's frames with "off").
- 0x2C4 / 0x54 D5–D6 relabeled as a likely temperature echo (previously "counter"); no behavior change.
- 0x60D D3–D5 documented as a likely controller ID/serial.
- Unmapped heater state `0x01` noted (single frame at end of cooldown on another B2L).
- D3 = `0x04` (power-up lock) added to the fault tables; controller-identity test results in `docs/experiments/controller-id.md`.

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
