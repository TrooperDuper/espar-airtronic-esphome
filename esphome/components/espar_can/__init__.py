"""
ESPHome external component — Espar Airtronic S3 CAN bus controller.
Trooper camper project.  WeAct CAN485 V1.0 (ESP32-D0WD-V3).

Exposes:
  - climate entity  (HEAT / FAN_ONLY / OFF) with external current-temp sensor
  - text_sensor     heater_state  ("STARTUP" / "IDLE" / "HEATING" / "FAN" / "UNKNOWN")
  - binary_sensor   flame_active  (D2 bit5 of 0x2C4)
  - binary_sensor   heater_connected  (0x625 heartbeat present)
  - text_sensor     fault_text    (human-readable fault / "OK")
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor, binary_sensor, text_sensor
from esphome.const import CONF_ID

# ── Component registration ────────────────────────────────────────────────
DEPENDENCIES = []
AUTO_LOAD   = ["climate", "sensor", "binary_sensor", "text_sensor"]

espar_can_ns = cg.esphome_ns.namespace("espar_can")
EsparCanComponent = espar_can_ns.class_(
    "EsparCanComponent", climate.Climate, cg.Component
)

# ── YAML keys ────────────────────────────────────────────────────────────
CONF_HEAT_SETPOINT_F            = "heat_setpoint_f"
CONF_CURRENT_TEMPERATURE_SENSOR = "current_temperature_sensor"
CONF_HEATER_STATE               = "heater_state"
CONF_FLAME_ACTIVE               = "flame_active"
CONF_HEATER_CONNECTED           = "heater_connected"
CONF_FAULT_TEXT                 = "fault_text"

# ── Config schema ─────────────────────────────────────────────────────────
# climate_schema(class) is the modern ESPHome API (replaces CLIMATE_SCHEMA).
# It registers the ID with the correct C++ class automatically.
CONFIG_SCHEMA = (
    climate.climate_schema(EsparCanComponent)
    .extend(
        {
            # Fixed high setpoint (°F) sent to the heater hardware.
            # Our own thermostat logic (current vs target) controls on/off.
            cv.Optional(CONF_HEAT_SETPOINT_F, default=85.0): cv.float_range(
                min=70.0, max=95.0
            ),
            # Averaged cabin temperature — typically a HA template sensor.
            cv.Optional(CONF_CURRENT_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            # Optional entity outputs
            cv.Optional(CONF_HEATER_STATE):     text_sensor.text_sensor_schema(),
            cv.Optional(CONF_FLAME_ACTIVE):     binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_HEATER_CONNECTED): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_FAULT_TEXT):       text_sensor.text_sensor_schema(),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


# ── Code generation ───────────────────────────────────────────────────────
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    cg.add(var.set_heat_setpoint_f(config[CONF_HEAT_SETPOINT_F]))

    if CONF_CURRENT_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_CURRENT_TEMPERATURE_SENSOR])
        cg.add(var.set_current_temperature_sensor(sens))

    if CONF_HEATER_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_HEATER_STATE])
        cg.add(var.set_heater_state_sensor(sens))

    if CONF_FLAME_ACTIVE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_FLAME_ACTIVE])
        cg.add(var.set_flame_sensor(sens))

    if CONF_HEATER_CONNECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_HEATER_CONNECTED])
        cg.add(var.set_connected_sensor(sens))

    if CONF_FAULT_TEXT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FAULT_TEXT])
        cg.add(var.set_fault_sensor(sens))
