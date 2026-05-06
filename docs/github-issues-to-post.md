# GitHub Issues — Draft

Post these manually on GitHub after creating the repository.
Each issue maps to a label — create labels first:
  bug, enhancement, good first issue, help wanted, documentation, research

---

## Issue 1 — Boot sync delay (~20s) requires controller restart on heater power-on

**Labels:** bug, known limitation
**Title:** Boot sync: ESP32 must restart when heater powers on (~20s delay before climate entity is ready)

**Body:**
The CAN handshake requires the ESP32 to send its init burst while the heater is actively responding. If the ESP32 boots before the heater has powered up and started its CAN polling, the init burst goes unanswered.

**Current workaround:** A relay wired to heater power triggers an ESP32 restart via the `Restart Espar Controller` button entity in HA. The heater takes ~20s to become available after power-on (ESP32 boot + WiFi + CAN handshake).

**Example HA automation:**
```yaml
trigger:
  - platform: state
    entity_id: binary_sensor.heater_power
    to: "on"
action:
  - delay: 2s
  - service: button.press
    target:
      entity_id: button.restart_espar_controller
```

**What a proper fix might look like:**
- Detect when the heater first appears on the bus (0x625 received after a cold boot) and automatically re-run the init burst without requiring a full ESP32 restart
- The component already retries the init burst every 150ms — the question is whether a mid-session heater power-on without a controller restart works in practice

**Contributions welcome:** If you test mid-session heater power-on without an ESP32 restart and find it works (or doesn't), please report.

---

## Issue 2 — Fault frame CAN IDs not decoded

**Labels:** research, help wanted
**Title:** Fault frame RE incomplete — P-codes known but CAN frame IDs not captured

**Body:**
The Eberspächer service manual lists P-codes and their CAN codes (e.g. P000307 = code 081), confirming that fault codes are exposed over CAN via the EasyScan diagnostic protocol. However, the specific CAN frame IDs used for fault reporting have not yet been captured.

The component currently uses behavioral detection only (startup timeout, heartbeat loss, failed start counter). This catches the most dangerous conditions but cannot surface specific P-codes to Home Assistant.

**See:** `docs/fault-codes.md` for the complete P-code table and capture methodology.

**Priority capture targets:**
1. P000307 — CAN communication error (easy to trigger: disconnect controller mid-session)
2. P00012A — Failed start (disconnect fuel line briefly)
3. P000110 — Air inlet sensor fault (unplug sensor)

**What's needed:** SavvyCAN captures with a specific fault triggered. New or changed frames relative to normal operation are the fault-reporting frames. Share captures in `captures/fault-codes/`.

---

## Issue 3 — Protocol compatibility unknown on non-B2L variants

**Labels:** research, help wanted
**Title:** Only tested on Airtronic S3 B2L Gasoline 12V — diesel and other variants untested

**Body:**
All captures and protocol decoding was done on a single unit: **Espar Airtronic S3 B2L Gasoline 12V**. It is unknown whether:

- The Airtronic S3 D2L (diesel) uses the same protocol
- Older S3 revisions use the same frame IDs and payloads
- The S2 series is compatible
- 24V variants differ

**CAN bus speed (500 kbps) is likely consistent** across variants as it's an Eberspächer platform standard, but command payloads and state bytes may differ.

**What's needed:** Anyone with a different variant running the listen sketch (`arduino/CAN_CSV_WeAct_Listen.ino`) and sharing captures. Even a short idle-state capture would let us compare 0x625 structure and 0x2C4 state bytes.

---

## Issue 4 — Heartbeat timeout occasionally fires false positives

**Labels:** bug
**Title:** Heater 0x625 heartbeat occasionally gaps >5s causing spurious "heartbeat lost" events

**Body:**
The heater's 0x625 heartbeat is expected at ~100ms intervals but occasionally has gaps longer than the component's timeout threshold. The timeout was increased from 5s to 10s as a workaround, which greatly reduces false positives but doesn't eliminate them entirely.

Observed behavior: "Heartbeat lost" logged, followed immediately (<100ms) by "Heartbeat received — connected" — indicating the heater was never truly offline.

**Possible causes:**
- CAN bus noise / error recovery causing frames to be dropped at the ESP32 receiver
- The heater itself pausing 0x625 during internal state transitions
- Bus-off recovery periods on the ESP32 TWAI controller

**Potential improvements:**
- Require N consecutive missed heartbeats before declaring offline (rather than a simple timeout)
- Track the rolling average heartbeat interval and set the timeout dynamically
- Add TWAI error counters to the diagnostic output to correlate with false positives

---

## Issue 5 — Temperature unit mismatch with Fahrenheit HA sensors

**Labels:** bug, good first issue
**Title:** Cabin temperature sensor assumed to be °C — users with °F HA sensors need a conversion filter

**Body:**
The ESPHome component's thermostat logic operates internally in Celsius. If the HA template sensor for cabin temperature reports in Fahrenheit, the numeric value flows into the climate entity as-is, causing the thermostat to compare e.g. `44.9°F` as if it were `44.9°C` (113°F) — and never command heat.

**Current fix:** An optional lambda filter in `espar-heater.yaml` converts °F → °C:
```yaml
- lambda: return (x - 32.0f) * 5.0f / 9.0f;
```

**What would be better:**
- A YAML config option (`cabin_temp_unit: fahrenheit`) that applies the conversion automatically in the component
- Or clearer documentation + a compile-time warning if the sensor unit doesn't match

This is a **good first issue** for someone comfortable with ESPHome component config schema.

---

## Issue 6 — Cannot connect OEM EasyStart Pro and WeAct simultaneously (P000342)

**Labels:** documentation, known limitation
**Title:** OEM controller and WeAct cannot be connected simultaneously — triggers P000342

**Body:**
The Eberspächer Airtronic S3 allows a maximum of 2 CAN controllers on the bus. Connecting the OEM EasyStart Pro for diagnostics while the WeAct is also connected triggers fault P000342 ("invalid configuration — too many CAN controllers") and may lock the control box.

**Workaround:** Disconnect the WeAct (physically unplug CAN H/L) before connecting the EasyStart Pro or any other OEM diagnostic tool. Reconnect the WeAct after OEM tools are removed.

**Enhancement request:** An HA service call or button entity that puts the component into a "safe standby" mode — stops all TX frames — without requiring a physical disconnect. This would make OEM service visits less friction-heavy.

---

## Issue 7 — No .dbc file for SavvyCAN drag-and-drop signal decoding

**Labels:** enhancement, good first issue
**Title:** Create a .dbc database file for SavvyCAN / CANalyzer

**Body:**
A `.dbc` (CAN database) file would allow anyone to load the decoded signal definitions into SavvyCAN, CANalyzer, or other tools and see decoded values (heater state, flame status, setpoint) directly on the bus view — no manual byte reading required.

The `tools/decode.py` script currently does annotation in a post-processing step. A proper `.dbc` file would be more useful for live capture sessions.

**What's needed:** A `.dbc` file defining:
- Message 0x2C4 with signals: HeaterState (D1), FlameActive (D2 bit5), Counter (D5/D6 LE)
- Message 0x54 with signals: CommandMode (D2), SetpointRaw (D3/D4 LE, factor 0.1°C)
- Message 0x625 with signals: AliveToggle (D2)

Good first issue for anyone familiar with `.dbc` format.

---

## Issue 8 — Compact enclosure design for WeAct + level shifter

**Labels:** enhancement
**Title:** [Nice-to-have] PCB or enclosure design for WeAct CAN485 + level shifter integration

**Body:**
The current setup requires a WeAct CAN485 board plus a separate logic level shifter module, connected by jumper wires. This works on a bench but is not ideal for a permanent vehicle installation.

A compact PCB or 3D-printed enclosure that integrates:
- WeAct CAN485 (or footprint for it)
- Level shifter
- Power connector (12V in with onboard 5V regulator)
- Molex MicroFit 3.0 connector for direct harness tap

would make this significantly more install-friendly. No contributions on this yet — sharing designs of any quality would be a good starting point.

---

## Issue 9 — No duration timer in HA for heater auto-off

**Labels:** enhancement, good first issue
**Title:** [Nice-to-have] Add configurable auto-off timer to ESPHome component or document HA approach

**Body:**
The Espar heater runs until commanded OFF — there is no duration parameter in the CAN protocol. The OEM EasyStart Pro provides a duration selector (30min, 42min, etc.) as a UI feature implemented in the controller, not in the heater.

Options for this project:
1. **HA automation approach** (simplest): document a `timer` helper + automation that turns off the climate entity after a set duration
2. **ESPHome approach**: add a `run_duration_minutes` config option to the component that starts an internal timer when HEAT is commanded and sends IDLE when it expires

A documented HA automation would be a good first issue. The ESPHome approach is a larger change but more self-contained.

**Example HA automation for auto-off:**
```yaml
trigger:
  - platform: state
    entity_id: climate.espar_heater
    to: heat
action:
  - delay: "01:00:00"   # 1 hour
  - service: climate.turn_off
    target:
      entity_id: climate.espar_heater
```

---

## Issue 10 — 0x625 session counter and D4–D8 bytes not decoded

**Labels:** research
**Title:** 0x625 bytes D3–D8 not fully decoded — possible serial/model information

**Body:**
The heater heartbeat frame (0x625) has the following structure in captures:
```
25 [toggle] [session_ctr] 86 01 54 5C 06
```

- D1 (`0x25`) — constant, meaning unknown
- D2 — alternates `0x00`/`0x10` (alive toggle, confirmed)
- D3 — increments slowly across captures (session counter, confirmed)
- D4–D8 (`86 01 54 5C 06`) — static across all captures; meaning unknown

The static bytes `54 5C` in D6/D7 match two of the init burst IDs (0x54 and 0x5C), which may be coincidence or may indicate a model/variant identifier.

**Research question:** Do these bytes differ between heater units, variants, or firmware versions? Sharing captures from different units would help determine if D4–D8 encode serial/model information or are simply constant for this platform.
