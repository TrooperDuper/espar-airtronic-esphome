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

### 4. Create a cabin temperature sensor in HA (optional but recommended)

The component works without a temperature sensor — it will heat continuously when mode=HEAT. For thermostat behavior, create a template sensor in HA that averages your cabin sensors:

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
5. Uses the averaged cabin temperature from your HA sensor to make thermostat decisions — heat when below `(target - 1.0°C)`, idle/fan when at or above target

The heater hardware always receives a fixed high setpoint (default 85°F, configurable). The component's thermostat logic decides when to send HEAT vs IDLE commands based on the cabin sensor. The heater's internal thermocouple acts only as a safety ceiling.

---

## Component configuration options

```yaml
espar_can:
  id: espar
  name: "Espar Heater"                  # required for HA discovery

  heat_setpoint_f: 85.0                 # fixed setpoint sent to heater hardware (°F)
                                        # range 70–95; default 85.0
                                        # our thermostat (cabin avg vs HA target) controls
                                        # on/off — the heater's thermocouple is just a safety ceiling

  cabin_temp_unit: celsius              # "celsius" (default) or "fahrenheit"
                                        # set to "fahrenheit" if your HA sensor reports °F;
                                        # the component converts internally — no lambda filter needed

  pause_mode: off                       # "off" (default) or "fan"
                                        # "off"  — send IDLE on setpoint reached; full ~4-min cooldown each cycle
                                        # "fan"  — send FAN_ONLY instead; blower runs without combustion
                                        #           (pseudo-pause, ~4W continuous); avoids repeated full stop/starts

  current_temperature_sensor: cabin_avg_temp   # optional; heats continuously if omitted

  heater_state:                         # optional text sensor: STARTUP/IDLE/HEATING/FAN/UNKNOWN
    name: "Espar Heater State"
  flame_active:                         # optional binary sensor: true when combustion confirmed
    name: "Espar Flame Active"
  heater_connected:                     # optional binary sensor: true when 0x625 heartbeat present
    name: "Espar Connected"
  fault_text:                           # optional text sensor: "OK" or fault description
    name: "Espar Fault"
```

---

## OEM diagnostics — CAN standby mode

Eberspächer supports multiple control elements on the CAN bus — each gets its own address via ADR coding (an EasyStart Pro and EasyStart Remote+ can run together). The WeAct currently *emulates the EasyStart Pro*, so running it alongside a real EasyStart Pro puts two devices with the same identity on the bus — an invalid configuration that can trigger fault **P000342** ("invalid configuration — check ADR coding"). This is an addressing collision, not a controller-count limit.

The `Espar CAN Standby` switch (defined in `espar-heater.yaml`) suspends all outbound frames from the WeAct without physically unplugging anything. Turning it ON from HA before connecting an OEM tool prevents the conflict. Turning it OFF afterwards automatically re-runs the CAN handshake.

---

## Climate entity behavior

| HA Mode | Command sent | Heater behavior |
|---|---|---|
| HEAT | HEAT at 85°F (configurable) when cabin < setpoint; IDLE or FAN_ONLY when above (see `pause_mode`) | Runs until cabin reaches setpoint |
| FAN ONLY | FAN command always | Blower runs, no combustion; ~75s cooldown after turning off |
| OFF | IDLE command always | Heater off |

---

## Entity reference

| Entity | Type | Description |
|---|---|---|
| `climate.espar_heater` | Climate | Main control entity |
| `sensor.espar_heater_state` | Text sensor | STARTUP / IDLE / HEATING / FAN / UNKNOWN |
| `binary_sensor.espar_flame_active` | Binary sensor | True when combustion confirmed |
| `binary_sensor.espar_connected` | Binary sensor | True when 0x625 heartbeat present |
| `sensor.espar_fault` | Text sensor | "OK" or fault description string |
| `switch.espar_can_standby` | Switch | Suspend all TX frames for OEM diagnostics (see below) |
| `light.espar_status_led` | Light | WS2812 RGB indicator |
| `button.restart_espar_controller` | Button | Restart the ESP32 (required for initial boot sync) |

> **Variant note:** `binary_sensor.espar_connected` latches on the heater's `0x625` heartbeat frame. This was validated on the **B2L (gasoline)** Airtronic S3; on other variants such as the diesel **D2L**, the heartbeat frame ID or cadence may differ, so connection detection may not behave identically and could need adjustment.

---

## LED color codes

| Color | Meaning |
|---|---|
| Dim green | Connected and idle |
| Amber (red+orange) | Heating active |
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

The most likely cause is a temperature unit mismatch. The component's thermostat logic operates in Celsius internally. If your HA cabin temperature sensor reports in Fahrenheit and `cabin_temp_unit` is not set to `fahrenheit`, the raw value flows in as-is — so `44.9°F` is treated as `44.9°C` (113°F), which is already above any reasonable setpoint, and the thermostat never commands heat.

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
