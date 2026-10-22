# ESPHome Integration

## Requirements

- ESPHome 2026.3 or later
- ESP32 board (tested on WeAct CAN485 V1.0 with `esp32dev` board config)
- `framework: type: arduino` — this config targets the Arduino framework (the status LED uses `esp32_rmt_led_strip`, which also supports ESP-IDF)

## Setup

### 1. Add the component

**Option A — From GitHub (recommended)**

Reference the component directly in your YAML. No files to copy; ESPHome fetches it at compile time:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/TrooperDuper/espar-airtronic-esphome
      ref: main
      path: esphome/components
    components: [espar_can]
```

**Option B — Local copy (for development)**

Copy the `components/` folder to the same directory as your ESPHome YAML:

```
your-esphome-config/
├── espar-heater.yaml
├── secrets.yaml
└── components/
    └── espar_can/
        ├── __init__.py
        ├── espar_can.h
        └── espar_can.cpp
```

And set the source in your YAML:
```yaml
external_components:
  - source:
      type: local
      path: components
    components: [espar_can]
```

### 2. Create secrets.yaml

```yaml
wifi_ssid: "YourNetwork"
wifi_password: "YourPassword"
api_encryption_key: "generate with: esphome generate-encryption-key"
ota_password: "choose-any-string"
fallback_hotspot_password: "choose-any-string"
```

### 3. Edit espar-heater.yaml

Update these values for your setup:

```yaml
sensor:
  - platform: homeassistant
    entity_id: sensor.cabin_average_temperature   # ← your HA sensor
```

If your HA cabin temperature sensor reports in **Fahrenheit**, set `cabin_temp_unit: fahrenheit` in the `espar_can:` block — the component converts °F→°C internally and no lambda filter is needed.

**Two example configs:**

| File | What it has |
|---|---|
| `espar-heater.yaml` | Minimal: the core entities, diagnostics only as comments |
| `espar-heater-full.yaml` | Everything: a select to switch the room sensor at run time, every diagnostic entity, *Max Run Time* as a number entity, and the web dashboard sorted into groups |

Both use the same hardware section and secrets (the full one needs no extra secrets).

### 4. Create a cabin temperature sensor in HA (optional)

The component works without a temperature sensor — the heater then regulates on its own intake sensor (`room_sensor: heater`, the default). To regulate on your cabin instead (`room_sensor: controller`), create a template sensor in HA that averages your cabin sensors:

```yaml
template:
  - sensor:
      - name: "Cabin Average Temperature"
        unit_of_measurement: "°F"          # or "°C" — must match cabin_temp_unit below
        device_class: temperature
        state: >
          {{ ( states('sensor.sensor_a') | float(0)
             + states('sensor.sensor_b') | float(0) ) / 2 }}
```

Then set `cabin_temp_unit` in your `espar_can:` block to match the unit your sensor reports:

```yaml
espar_can:
  cabin_temp_unit: fahrenheit   # or celsius (default)
```

### 5. Flash

First flash must be via USB:
```bash
esphome run espar-heater.yaml
```

Subsequent updates can be OTA.

---

## How it works

The `espar_can` component:

1. Sends the init burst on boot and retries every 150ms until the heater's 0x625 heartbeat is received
2. Sends continuous command frames (0x54/55/56/57 at 200ms, 0x60D heartbeat at 100ms)
3. Monitors 0x2C4 for heater state and 0x625 for connection health
4. Exposes a `climate` entity to Home Assistant with HEAT / FAN_ONLY / OFF modes
5. In HEAT, sends the climate target as the heater's setpoint together with a room temperature; the heater starts, modulates, pauses and restarts by itself (see [Regulation](#regulation))

The component makes no on/off decisions in HEAT — like the OEM EasyStart Pro, it only tells the heater what the target and the room temperature are.

---

## Component configuration options

```yaml
espar_can:
  id: espar
  name: "Espar Heater"                  # required for HA discovery

  cabin_temp_unit: celsius              # "celsius" (default) or "fahrenheit"
                                        # set to "fahrenheit" if your HA sensor reports °F;
                                        # the component converts internally — no lambda filter needed

  room_sensor: heater                   # the room temperature the heater regulates on — see "Regulation"
                                        # "heater" (default, its own intake sensor)
                                        # or "controller" (current_temperature_sensor)
  current_temperature_sensor: cabin_avg_temp   # optional; only used with room_sensor: controller

  max_run_time: 120min                  # default 120min; 0min = no limit. HEAT or FAN ONLY is switched
                                        # off after this time, however it was started — the heater
                                        # has no limit of its own. Run-time change: set_max_run_time_minutes()

  auto_clear_idle_lock: true            # default true. After a heater power interruption the heater
                                        # often sits in IDLE with fault byte D3=0x04 and ignores
                                        # HEAT/FAN. When true, the component releases it (45 s wait,
                                        # FAN 6 s, IDLE 5 s, up to 3 tries). See docs/fault-codes.md.

  controller_id: 0x82B609               # optional; 24-bit ID sent in 0x60D D3–D5 (default shown)
  init_0x62_byte: 0xF1                  # optional; last byte of init frame 0x62 (default shown)
                                        # The heater accepted every value tested; leave as-is unless
                                        # you're doing multi-controller experiments.

  heater_state:                         # optional text sensor: STARTUP/IDLE/HEATING/FAN/COOLDOWN/UNKNOWN
    name: "Espar Heater State"
  flame_active:                         # optional binary sensor: true while heater reports HEATING (0x2C4 D1=0x09)
    name: "Espar Heating"
  flag_0x20:                            # optional diagnostic: raw 0x2C4 D2 bit5, meaning unconfirmed
    name: "Espar Status Flag 0x20"
  heater_connected:                     # optional binary sensor: true when 0x625 heartbeat present
    name: "Espar Connected"
  fault_text:                           # optional text sensor: "OK" or fault description
    name: "Espar Fault"

  diagnostics:                          # optional, read-only heater diagnostics — off unless present
    update_interval: 10s                # see "Heater diagnostics" below
    flame_detected:
      name: "Espar Flame"
```

---

## After a heater power interruption (IDLE lock)

If the heater's supply is interrupted (battery disconnect, low-voltage cutoff, pulled fuse), it often comes back in IDLE with fault byte D3 = `0x04` and **ignores commands**. *Espar Fault* shows `IDLE LOCK: D3=0x04 (heater ignoring commands)`. With `auto_clear_idle_lock: true` (default) the component releases it automatically about 45–60 s after the heater reconnects, and your HEAT/FAN request resumes. If all three attempts fail, *Espar Fault* says so. Then set the mode Off → Fan only (wait ~5 s) → Off, or power the heater off for a few minutes. Details: [#14](https://github.com/TrooperDuper/espar-airtronic-esphome/issues/14), [docs/fault-codes.md](../docs/fault-codes.md).

---

## OEM diagnostics — CAN standby mode

If another controller (e.g. a real EasyStart Pro) appears on the bus while the WeAct is transmitting, the component sees its 0x60D or 0x54–0x57 frames and sets *Espar Fault* = `OTHER CONTROLLER ON BUS — enable CAN Standby`. Its frames (0x54–0x57, 0x60D, 0x65 and the init burst) are logged at debug level, not raised as unknown-ID faults.

The WeAct currently *emulates the EasyStart Pro*: it sends the same CAN IDs (0x54–0x57, 0x60D, 0x65). With a real Pro also transmitting, the heater raises fault **P000342**, "invalid configuration: too many CAN units" (wording from Eberspächer's Hydronic S3 repair instructions, via [@farnham26](https://github.com/farnham26); see [docs/fault-codes.md](../docs/fault-codes.md)). The heater accepts one control unit at a time (and on [@farnham26](https://github.com/farnham26)'s Hydronic, the EasyStart answered another controller's frames by sending "off"). (The "ADR" sometimes shown with it is Eberspächer's dangerous-goods mode, not a bus address; thanks to Greg ([@gregoryxxxxxx](https://github.com/gregoryxxxxxx)) for spotting it.) Turn on CAN Standby whenever a real Pro is on the bus.

The `Espar CAN Standby` switch (defined in `espar-heater.yaml`) suspends all outbound frames from the WeAct without physically unplugging anything. Turning it ON from HA before connecting an OEM tool prevents the conflict. Turning it OFF afterwards automatically re-runs the CAN handshake.

---

## Heater diagnostics (optional, read-only)

The heater answers diagnostic requests on the CAN bus (UDS on 0x7A0 → 0x73C, the channel EasyScan and the EasyStart Pro's workshop menu use). With a `diagnostics:` block the component asks for values the cyclic frames don't carry: real flame detection, flame sensor temperature, supply voltage, fuel pump, glow plug, combustion phase, fuel used, the heater's own operating hours and its fault memory with P-codes.

- **Off by default.** Without a `diagnostics:` block nothing is ever requested.
- **Read-only.** Only ReadDataByIdentifier (`22`) and ReadDTCInformation (`19 02 08`). Nothing is written, reset or cleared.
- **Quiet.** One request at a time, 50 ms apart. A sweep only asks for what the configured entities need, every `update_interval` (default 10 s) while the heater is starting, running or cooling down, once a minute while it idles. Versions and part number are read once.
- **Nothing in CAN Standby** and nothing while the heater isn't connected.
- The fault memory is read on connect, when the heater reports a fault, and after each run. To read it on demand: `- lambda: id(espar).read_fault_memory();`
- An identifier the heater rejects is not asked again.

Tested on an Airtronic S3 B2L (gasoline, software 02.03.11) in idle, fan, heating and cooldown. Identifiers, scalings and what is confirmed: [docs/uds-diagnostics.md](../docs/uds-diagnostics.md). Every entity ready to use: `espar-heater-full.yaml`.

```yaml
espar_can:
  # ...
  diagnostics:
    update_interval: 10s                # while active; min 2s. Idle: once a minute
    flame_detected:                     # binary: the heater's own flame detection (FD0F)
      name: "Espar Flame"
    combustion_phase:                   # text: Off / Startup / Pre-glow / Ignition / Flame ramp-up /
      name: "Espar Phase"               #       Burning / Control pause / Cooldown / Ventilation
    flame_temperature:                  # °C, flame sensor
      name: "Espar Flame Temperature"
    supply_voltage:                     # V
      name: "Espar Supply Voltage"
    fuel_pump_frequency:                # Hz
      name: "Espar Fuel Pump"
    fuel_rate:                          # l/h, 0 without flame
      name: "Espar Fuel Rate"
    fuel_total:                         # l, lifetime as the heater counts it
      name: "Espar Fuel Total"
    operating_hours:                    # h, the heater's own counter
      name: "Espar Operating Hours"
    fault_memory:                       # text: "P000115, P000129 (active)" or "None"
      name: "Espar Fault Memory"
```

All diagnostic entities (each optional):

| Key | Type | Source | Description |
|---|---|---|---|
| `flame_detected` | Binary sensor | FD0F | Flame detected by the heater (~70 s after the heat command) |
| `combustion_state` | Text sensor | FD0F | `No fuel` / `Fuel on` |
| `combustion_phase` | Text sensor | FD10 | Phase name, or the raw code (`0x..`) if unknown |
| `phase_step`, `phase_time` | Sensor | FD10 | Step within the start-up or cooldown sequence, seconds in it |
| `flame_temperature` | Sensor (°C) | FD01 | Flame sensor |
| `intake_temperature` | Sensor (°C) | FD01 | Intake air sensor |
| `heater_setpoint`, `heater_temperature` | Sensor (°C) | FD45 | Setpoint and temperature the heater itself is regulating on |
| `power_demand` | Sensor (%) | FD0E | Power demand while burning |
| `burn_time` | Sensor (s) | FD2A | Burn time of the current run |
| `supply_voltage` | Sensor (V) | FD04 | Supply voltage at the heater |
| `fuel_pump_frequency` | Sensor (Hz) | FD0D | Fuel metering pump |
| `glow_plug`, `glow_plug_power` | Binary sensor, sensor (W) | FD0A | Glow plug on / power (unit as on the Hydronic S3) |
| `blower_level` | Sensor | FD08 | Blower drive value, 0 = stopped (unit unknown) |
| `fuel_rate`, `fuel_total` | Sensor (l/h, l) | FD11 | Fuel rate; lifetime fuel |
| `fuel_pump_strokes` | Sensor | FD6A | Lifetime fuel pump strokes |
| `start_stop_count` | Sensor | FD06 | +1 at every start and every burner stop |
| `operating_hours` | Sensor (h) | FD17 | Operating time (fan, heating, cooldown) |
| `hours_power_0_25` … `hours_power_75_100`, `hours_control_pause`, `hours_altitude_mode` | Sensor (h) | FD17 | Time per power band, in control pause and in altitude mode |
| `control_unit_temperature` | Sensor (°C) | FD35 | Likely the control unit's temperature (unconfirmed) |
| `air_pressure` | Sensor (hPa) | FD36 | Likely the altitude-compensation sensor (unconfirmed) |
| `software_version`, `hardware_version`, `part_number` | Text sensor | F195, F193, F1F0 | Read once |
| `fault_memory` | Text sensor | `19 02 08` | Stored P-codes, `(active)` if active now; `None` if empty. Meanings: [docs/fault-codes.md](../docs/fault-codes.md) |

> **Heads-up:** during the heater's own control pause (phase `Control pause`, FD10 = `0x28`) the 0x2C4 state still says HEATING. *Espar Heating* and *Espar Heater State* stay on although nothing burns; `flame_detected` and `combustion_phase` show what's really happening.

---

## Regulation

The heater regulates by itself, as it does with the OEM EasyStart Pro: in HEAT the component sends HEAT continuously with the **climate target as the setpoint** and a **room temperature**, and the heater decides when to burn, how hard, and when to pause. The component makes no on/off decisions — so there is no short-cycling (#9): no full cooldown and new glow-plug ignition every time the cabin reaches the target.

| | |
|---|---|
| Setpoint sent (0x54 D3/D4) | the climate entity's target, °C × 10 |
| Room temperature sent (0x54 D5/D6, 0x65 D1/D2) | a real room temperature, (°C + 50) × 10 — see `room_sensor` |
| Who switches | the heater: starts, modulates (power demand 100 → 0 %), pauses, restarts |

> **Removed:** the component's own thermostat (`regulation: thermostat`) and its options `heat_setpoint_f`, `pause_mode`, `hysteresis`, `min_run_time`, `min_off_time`, `max_starts_per_hour`. A config that still has one of them fails validation with a message saying so — delete the option.

Where the room temperature comes from (`room_sensor`):

| `room_sensor` | Room temperature sent to the heater |
|---|---|
| `heater` (default) | The heater's own intake sensor, read over UDS (FD01, read-only) minus 3.0 K — the same correction the heater applies to its own value. Works without a `diagnostics:` block. |
| `controller` | `current_temperature_sensor`, sent as the control unit's sensor (like the Pro's built-in sensor). Needs `current_temperature_sensor`; without it the heater's sensor is used. |

> **Intake air:** `heater` measures the air the heater draws in. That is the cabin temperature only when the heater recirculates cabin air. With a fresh-air intake it would regulate on the outdoor temperature — use `controller` then.

> **Lost cabin temperature:** with `controller`, the heater's own sensor takes over while the value is missing (HA entity unavailable) or stale — no update and no HA connection for 5 min. An HA sensor only sends changes, so a steady value is fine as long as HA is connected. The log says so and *Espar Fault* shows `CABIN TEMP LOST — using the heater's own sensor` until the value is back. This keeps a WiFi or HA outage from leaving the heater on an old, cold value at full output. Don't put `filter_out: nan` on the cabin sensor: it would hide *unavailable* from the component.

It can be switched at run time, e.g. from a template select (as in `espar-heater-full.yaml`):

```yaml
select:
  - platform: template
    name: "Espar Room Sensor"
    optimistic: true
    restore_value: true
    options: ["Heater", "External"]
    on_value:
      - lambda: id(espar).set_room_sensor_controller(x == "External");
```

What to expect (Airtronic S3 B2L): with the setpoint below the room temperature the heater runs its blower for ~15 s, then goes into its **control pause** (no flame, blower off). Raising the setpoint makes it ignite by itself; lowering it while burning reduces the power demand. The heater's control is PI-like: a 1 K setpoint step moves the power demand by ~10 % at once, then it keeps drifting by ~0.5 % per 10 s and kelvin of error — up while below the setpoint, down while above, standing still at the setpoint. Above the setpoint while burning it runs down to 0 % demand and then stays at minimum output (fuel pump ~2.1 Hz) rather than pausing, as long as the room doesn't keep warming (55 min run, room 2–3 K above the setpoint for 16 min, no pause). The climate action shows *Idle* during the pause (detected via FD10 phase `0x28`), although 0x2C4 still reports HEATING — *Espar Heating* stays on meanwhile. The heater does not limit its own run time; the component switches HEAT or FAN ONLY off after `max_run_time` (default 120 min, `0min` = no limit; `espar-heater-full.yaml` sets it from a number entity). Background and measurements: [docs/experiments/heater-regulation.md](../docs/experiments/heater-regulation.md).

> Don't try to make the heater use its own sensor by sending "no value" (`FE FF`) with sensor selection `00`: it then regulates on 0.0 °C and keeps heating. The component always sends a real value.

---

## Climate entity behavior

| HA Mode | Command sent | Heater behavior |
|---|---|---|
| HEAT | HEAT continuously with the target as setpoint and a room temperature | Heater modulates, pauses and restarts by itself |
| FAN ONLY | FAN command always | Blower runs, no combustion; ~75s cooldown after turning off |
| OFF | IDLE command always | Heater off |

---

## Entity reference

| Entity | Type | Description |
|---|---|---|
| `climate.espar_heater` | Climate | Main control entity |
| `sensor.espar_heater_state` | Text sensor | STARTUP / IDLE / HEATING / FAN / COOLDOWN / UNKNOWN. COOLDOWN = after-run (blower purging, ~4 min after heating); the climate action shows *Fan* meanwhile. |
| `binary_sensor.espar_heating` | Binary sensor | True while the heater reports HEATING (0x2C4 D1=0x09), including the ignition ramp — not a combustion confirmation. YAML key: `flame_active`. Formerly `binary_sensor.espar_flame_active` (see Upgrading below). |
| `binary_sensor.espar_status_flag_0x20` | Binary sensor | Raw 0x2C4 D2 bit5 — diagnostic, meaning unconfirmed |
| `binary_sensor.espar_connected` | Binary sensor | True when 0x625 heartbeat present |
| `sensor.espar_fault` | Text sensor | "OK" or fault description string |
| `switch.espar_can_standby` | Switch | Suspend all TX frames for OEM diagnostics (see below) |
| `light.espar_status_led` | Light | WS2812 RGB indicator |
| `button.restart_espar_controller` | Button | Restart the ESP32 (required for initial boot sync) |

> **Variant note:** `binary_sensor.espar_connected` latches on the heater's `0x625` heartbeat frame. This was validated on the **B2L (gasoline)** Airtronic S3; on other variants such as the diesel **D2L**, the heartbeat frame ID or cadence may differ, so connection detection may not behave identically and could need adjustment.

---

## Upgrading

### "Espar Flame Active" renamed to "Espar Heating" (Unreleased)

The `flame_active` binary sensor now follows the heater's HEATING state (`0x2C4 D1=0x09`) rather than `D2` bit 5. HEATING starts within ~140 ms of the heat command — before the glow plug has lit anything — so "flame active" overstated it. The example config now names it **"Espar Heating"**.

The YAML key is still `flame_active`, so your config keeps validating. But Home Assistant derives an ESPHome entity's identity from its **name**, so renaming it creates a new entity and orphans the old one:

| | Entity ID (default) | After upgrade |
|---|---|---|
| Old | `binary_sensor.espar_flame_active` | Stays in HA as **unavailable** ("no longer being provided by the esphome integration") |
| New | `binary_sensor.espar_heating` | Created on first connect after flashing |

Your exact IDs may differ if you renamed entities or your device name is prefixed onto them; check **Settings → Devices & services → ESPHome → Espar Heater**.

**To upgrade:**

1. In your YAML, change `name: "Espar Flame Active"` to `name: "Espar Heating"` under `flame_active:` and flash. (Skip this step and keep the old name if you'd rather not migrate. The sensor's behavior changes either way; only the name differs.)
2. In HA, go to **Settings → Devices & services → Entities**, search for `espar_flame_active`, open it, and choose **Delete** (it shows as unavailable/not provided). Or select it and use **Remove selected**.
3. Update anything that referenced the old entity: automations, scripts, dashboards, template sensors. HA's **Repairs** panel and the entity's *Related* tab show what still points at it.
4. Optional: to keep your automations untouched instead of editing them, delete the old entity first (step 2), then open the new **Espar Heating** entity's settings and change its entity ID to `binary_sensor.espar_flame_active`. Its long-term statistics history starts fresh either way.
5. Optional: the example config also changes this sensor's `device_class` from `heat` to `running`, so HA shows **Running / Not running** instead of **Hot / Normal**. This only changes the state label, not the entity ID.

The diagnostic `flag_0x20` sensor ("Espar Status Flag 0x20") is new and only appears if you add it to your YAML.

---

## LED color codes

| Color | Meaning |
|---|---|
| Dim green | Connected and idle |
| Amber (red+orange) | Heating active |
| Dim orange | Cooldown (after-run blower) |
| Blue | Fan only |
| Red pulsing | Fault / error |
| Off | Not connected to heater |

---

## GPIO pins

| GPIO | Function |
|---|---|
| 26 | CAN RX |
| 27 | CAN TX |
| 4 | WS2812 LED data |

---

## Common pitfalls

**Heater won't start heating even with mode set to HEAT**

First check whether the room is already warmer than the target: the heater then goes into its control pause (climate action *Idle*, phase `Control pause`) — that's correct. With `room_sensor: controller` the other likely cause is a temperature unit mismatch. The component works in Celsius internally. If your HA cabin temperature sensor reports in Fahrenheit and `cabin_temp_unit` is not set to `fahrenheit`, the raw value flows in as-is — so `44.9°F` is sent as `44.9°C` (113°F), which is already above any reasonable setpoint, and the heater pauses.

Check your ESPHome logs for a line like:
```
Current Temperature: 44.95°C  Target Temperature: 21.00°C  Action: IDLE
```
If the current temperature looks implausibly high, add `cabin_temp_unit: fahrenheit` to your `espar_can:` block:
```yaml
espar_can:
  cabin_temp_unit: fahrenheit
```
This replaces the lambda filter approach that was used in earlier versions.

**Climate entity not appearing in Home Assistant**

Make sure the `espar_can:` block in your YAML includes a `name:` field:
```yaml
espar_can:
  id: espar
  name: "Espar Heater"   # ← required for HA discovery
```

**"Heartbeat lost" messages appearing every 60–90 seconds**

The heater occasionally gaps its 0x625 heartbeat by more than the timeout threshold. This is expected behavior — the component reconnects automatically within 100ms. If the messages are more frequent or the component does not recover, check CAN H/L wiring and connections at the XB10 harness tap.

**OTA flash fails after changing framework type**

ESPHome cannot OTA from `esp-idf` to `arduino` framework (or vice versa). If you change the framework type you must flash via USB. Connect the WeAct via USB-C and run `esphome run espar-heater.yaml` from your computer.
