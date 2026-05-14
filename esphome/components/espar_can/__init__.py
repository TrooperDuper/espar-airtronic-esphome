"""
ESPHome external component — Espar Airtronic S3 CAN bus controller.
Trooper camper project.  WeAct CAN485 V1.0 (ESP32-D0WD-V3).

Exposes:
  - climate entity  (HEAT / FAN_ONLY / OFF) with external current-temp sensor
  - text_sensor     heater_state  ("STARTUP" / "IDLE" / "HEATING" / "FAN" / "UNKNOWN")
  - binary_sensor   flame_active  (D2 bit5 of 0x2C4)
  - binary_sensor   heater_connected  (0x625 heartbeat present)
  - text_sensor     fault_text    (human-readable fault / "OK")

New config options (see CONFIG_SCHEMA below):
  cabin_temp_unit   — "celsius" (default) or "fahrenheit"; handles °F→°C
                      conversion internally, no lambda filter needed in YAML.
  pause_mode        — "off" (default) or "fan"; controls heater behaviour
                      when cabin reaches setpoint.  "fan" keeps the blower
                      running (pseudo-pause) instead of a full shutdown.
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
CONF_CABIN_TEMP_UNIT            = "cabin_temp_unit"
CONF_PAUSE_MODE                 = "pause_mode"
CONF_HEATER_STATE               = "heater_state"
CONF_FLAME_ACTIVE               = "flame_active"
CONF_HEATER_CONNECTED           = "heater_connected"
CONF_FAULT_TEXT                 = "fault_text"

# ── Custom validators ─────────────────────────────────────────────────────
# YAML parses bare `off` as boolean False before ESPHome sees it.
# This validator accepts both the boolean form and the quoted string form.
def _validate_pause_mode(value):
    if value is False or value == "off":
        return "off"
    if value is True or value == "fan":
        return "fan"
    raise cv.Invalid(f"Invalid pause_mode '{value}': must be 'off' or 'fan' (quote the value in YAML)")

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
            # Unit of the cabin temperature sensor.
            # "celsius"    (default) — value passed through unchanged.
            # "fahrenheit" — component converts °F→°C internally before the
            #                thermostat comparison.  Remove any lambda filter
            #                from the HA sensor block when using this option.
            cv.Optional(CONF_CABIN_TEMP_UNIT, default="celsius"): cv.one_of(
                "celsius", "fahrenheit", lower=True
            ),
            # Behaviour when the cabin reaches the target setpoint.
            # "off" (default) — send IDLE; heater does full 4-min cooldown
            #                   and glow-plug clean on every cycle.
            # "fan"           — send FAN_ONLY; blower keeps running without
            #                   combustion (pseudo-pause mode).  Avoids the
            #                   full stop/start cycle.  Draws ~4W continuous
            #                   battery power while paused.
            cv.Optional(CONF_PAUSE_MODE, default="off"): _validate_pause_mode,
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
    cg.add(var.set_cabin_temp_fahrenheit(config[CONF_CABIN_TEMP_UNIT] == "fahrenheit"))
    cg.add(var.set_pause_mode_fan(config[CONF_PAUSE_MODE] == "fan"))

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
