# ESPHome Integration

## Requirements

- ESPHome 2026.3 or later
- ESP32 board (tested on WeAct CAN485 V1.0 with `esp32dev` board config)
- `framework: type: arduino` — required for the neopixelbus LED component

## Setup

### 1. Copy the component

Copy the `components/` folder to the same directory as your ESPHome YAML config:

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

If your HA cabin temperature sensor reports in **Fahrenheit**, uncomment the lambda filter in the sensor config to convert to Celsius before the thermostat compares it against the setpoint.

### 4. Create a cabin temperature sensor in HA (optional but recommended)

The component works without a temperature sensor — it will heat continuously when mode=HEAT. For thermostat behavior, create a template sensor in HA that averages your cabin sensors:

```yaml
template:
  - sensor:
      - name: "Cabin Average Temperature"
        unit_of_measurement: "°C"
        device_class: temperature
        state: >
          {{ ( states('sensor.sensor_a') | float(0)
             + states('sensor.sensor_b') | float(0) ) / 2 }}
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
5. Uses the averaged cabin temperature from your HA sensor to make thermostat decisions — heat when below `(target - 0.3°C)`, idle when at or above target

The heater hardware always receives a fixed high setpoint (default 85°F, configurable). The component's thermostat logic decides when to send HEAT vs IDLE commands based on the cabin sensor. The heater's internal thermocouple acts only as a safety ceiling.

---

## Climate entity behavior

| HA Mode | Command sent | Heater behavior |
|---|---|---|
| HEAT | HEAT at 85°F (configurable) when cabin < setpoint; IDLE when above | Runs until cabin reaches setpoint |
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
| `light.espar_status_led` | Light | WS2812 RGB indicator |

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
