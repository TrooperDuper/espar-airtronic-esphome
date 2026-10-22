"""
ESPHome external component — Espar Airtronic S3 CAN bus controller.
Trooper camper project.  WeAct CAN485 V1.0 (ESP32-D0WD-V3).

Exposes:
  - climate entity  (HEAT / FAN_ONLY / OFF). HEAT sends the target to the heater,
                    which starts, modulates, pauses and restarts by itself
  - text_sensor     heater_state  ("STARTUP" / "IDLE" / "HEATING" / "FAN" / "COOLDOWN" / "UNKNOWN")
  - binary_sensor   flame_active  ("Espar Heating": true while heater state D1=0x09 HEATING,
                                   including ignition)
  - binary_sensor   flag_0x20     (raw 0x2C4 D2 bit5 — diagnostic, meaning unconfirmed)
  - binary_sensor   heater_connected  (0x625 heartbeat present)
  - text_sensor     fault_text    (human-readable fault / "OK")

Optional `diagnostics:` block — read-only UDS requests on 0x7A0 → 0x73C. Without it
only the intake air and the combustion phase are requested (the heater's own room
temperature and its control pause), and not with room_sensor: controller. Nothing is written, reset or cleared; nothing is sent in
CAN standby. See docs/uds-diagnostics.md. Entities (all optional):
  - binary_sensor   flame_detected            (the heater's own flame detection)
  - text_sensor     combustion_state          ("No fuel" / "Fuel on")
  - text_sensor     combustion_phase          ("Startup" / "Pre-glow" / "Ignition" / "Burning" /
                                               "Control pause" / "Cooldown" / ...)
  - sensor          phase_step, phase_time    (step within the phase, seconds in it)
  - sensor          flame_temperature, intake_temperature    (°C)
  - sensor          supply_voltage            (V)
  - sensor          fuel_pump_frequency       (Hz)
  - binary_sensor   glow_plug;  sensor glow_plug_power (W)
  - sensor          blower_level              (drive value, 0 = stopped, unit unknown)
  - sensor          fuel_rate (l/h), fuel_total (l, lifetime as the heater counts it)
  - sensor          power_demand              (%, while burning)
  - sensor          burn_time                 (s, current run)
  - sensor          heater_setpoint, heater_temperature  (°C, what the heater regulates on)
  - sensor          control_unit_temperature  (°C, unconfirmed), air_pressure (hPa, unconfirmed)
  - sensor          operating_hours, hours_power_0_25 … hours_power_75_100,
                    hours_control_pause, hours_altitude_mode  (h, the heater's own counters)
  - sensor          start_stop_count, fuel_pump_strokes
  - text_sensor     software_version, hardware_version, part_number
  - text_sensor     fault_memory              (stored P-codes, "None" when empty)

Config options (see CONFIG_SCHEMA below):
  cabin_temp_unit   — "celsius" (default) or "fahrenheit"; handles °F→°C
                      conversion internally, no lambda filter needed in YAML.
  room_sensor       — which room temperature the heater regulates on:
                      "heater" (default, its own intake sensor read over UDS) or
                      "controller" (current_temperature_sensor; the heater's own
                      sensor takes over while that is unavailable or stale).
  max_run_time      — HEAT / FAN_ONLY are switched off after this time (default
                      120min; 0min = no limit). The heater has no limit of its own.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor, binary_sensor, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_ATMOSPHERIC_PRESSURE,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_VOLUME,
    DEVICE_CLASS_VOLUME_FLOW_RATE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_HECTOPASCAL,
    UNIT_HERTZ,
    UNIT_HOUR,
    UNIT_LITRE,
    UNIT_PERCENT,
    UNIT_SECOND,
    UNIT_VOLT,
    UNIT_WATT,
)

# ── Component registration ────────────────────────────────────────────────
DEPENDENCIES = []
AUTO_LOAD   = ["climate", "sensor", "binary_sensor", "text_sensor"]

espar_can_ns = cg.esphome_ns.namespace("espar_can")
EsparCanComponent = espar_can_ns.class_(
    "EsparCanComponent", climate.Climate, cg.Component
)

# ── YAML keys ────────────────────────────────────────────────────────────
CONF_CURRENT_TEMPERATURE_SENSOR = "current_temperature_sensor"
CONF_CABIN_TEMP_UNIT            = "cabin_temp_unit"
CONF_ROOM_SENSOR                = "room_sensor"
CONF_MAX_RUN_TIME               = "max_run_time"
CONF_CONTROLLER_ID              = "controller_id"
CONF_AUTO_CLEAR_IDLE_LOCK       = "auto_clear_idle_lock"
CONF_INIT_0X62_BYTE             = "init_0x62_byte"
CONF_HEATER_STATE               = "heater_state"
CONF_FLAME_ACTIVE               = "flame_active"
CONF_FLAG_0X20                  = "flag_0x20"
CONF_HEATER_CONNECTED           = "heater_connected"
CONF_FAULT_TEXT                 = "fault_text"
CONF_DIAGNOSTICS                = "diagnostics"

# ── Removed options ───────────────────────────────────────────────────────
# The component no longer switches the heater on and off (#9): the heater gets the
# climate target and regulates by itself. Old configs get a clear message.
_REMOVED = (
    "removed: the heater regulates to the climate target by itself (modulates, pauses), "
    "so the component no longer switches it on and off. Delete this option. "
    "See 'Regulation' in esphome/README.md."
)
REMOVED_OPTIONS = (
    "heat_setpoint_f",
    "pause_mode",
    "regulation",
    "hysteresis",
    "min_run_time",
    "min_off_time",
    "max_starts_per_hour",
)

# ── Diagnostics (UDS, read-only) ──────────────────────────────────────────
def _temperature(**kw):
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE, state_class=STATE_CLASS_MEASUREMENT, **kw)

def _hours():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_HOUR, accuracy_decimals=1, device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_TOTAL_INCREASING, entity_category=ENTITY_CATEGORY_DIAGNOSTIC)

def _count():
    return sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC)

def _diag_text():
    return text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)

# key → (schema, C++ setter)
DIAG_SENSORS = {
    "flame_temperature":   (_temperature(), "set_flame_temperature_sensor"),
    "intake_temperature":  (_temperature(), "set_intake_temperature_sensor"),
    "heater_setpoint":     (_temperature(), "set_heater_setpoint_sensor"),
    "heater_temperature":  (_temperature(), "set_heater_temperature_sensor"),
    "control_unit_temperature": (_temperature(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
                                 "set_control_unit_temperature_sensor"),
    "supply_voltage": (sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT, accuracy_decimals=1, device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "set_supply_voltage_sensor"),
    "fuel_pump_frequency": (sensor.sensor_schema(
        unit_of_measurement=UNIT_HERTZ, accuracy_decimals=1, device_class=DEVICE_CLASS_FREQUENCY,
        state_class=STATE_CLASS_MEASUREMENT, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "set_fuel_pump_frequency_sensor"),
    "glow_plug_power": (sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT, accuracy_decimals=1, device_class=DEVICE_CLASS_POWER,
        state_class=STATE_CLASS_MEASUREMENT, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "set_glow_plug_power_sensor"),
    "phase_step": (sensor.sensor_schema(
        accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC), "set_phase_step_sensor"),
    "phase_time": (sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND, accuracy_decimals=0, device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_MEASUREMENT, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        "set_phase_time_sensor"),
    "blower_level": (sensor.sensor_schema(
        accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC), "set_blower_level_sensor"),
    "fuel_rate": (sensor.sensor_schema(
        unit_of_measurement="L/h", accuracy_decimals=2, device_class=DEVICE_CLASS_VOLUME_FLOW_RATE,
        state_class=STATE_CLASS_MEASUREMENT), "set_fuel_rate_sensor"),
    "fuel_total": (sensor.sensor_schema(
        unit_of_measurement=UNIT_LITRE, accuracy_decimals=3, device_class=DEVICE_CLASS_VOLUME,
        state_class=STATE_CLASS_TOTAL_INCREASING), "set_fuel_total_sensor"),
    "power_demand": (sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT, accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT), "set_power_demand_sensor"),
    "burn_time": (sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND, accuracy_decimals=0, device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_MEASUREMENT), "set_burn_time_sensor"),
    "air_pressure": (sensor.sensor_schema(
        unit_of_measurement=UNIT_HECTOPASCAL, accuracy_decimals=1,
        device_class=DEVICE_CLASS_ATMOSPHERIC_PRESSURE, state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC), "set_air_pressure_sensor"),
    "operating_hours":     (_hours(), "set_operating_hours_sensor"),
    "start_stop_count":    (_count(), "set_start_stop_count_sensor"),
    "fuel_pump_strokes":   (_count(), "set_fuel_pump_strokes_sensor"),
}
DIAG_BINARY_SENSORS = {
    "flame_detected": (binary_sensor.binary_sensor_schema(device_class=DEVICE_CLASS_RUNNING),
                       "set_flame_detected_sensor"),
    "glow_plug": (binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
                  "set_glow_plug_sensor"),
}
DIAG_TEXT_SENSORS = {
    "combustion_phase": (text_sensor.text_sensor_schema(), "set_combustion_phase_sensor"),
    "combustion_state": (_diag_text(), "set_combustion_state_sensor"),
    "software_version": (_diag_text(), "set_software_version_sensor"),
    "hardware_version": (_diag_text(), "set_hardware_version_sensor"),
    "part_number":      (_diag_text(), "set_part_number_sensor"),
    # Stored fault codes ("P000115, P000129 (active)" / "None"), read on connect,
    # on a fault and after each run.
    "fault_memory":     (_diag_text(), "set_fault_memory_sensor"),
}
# FD17 words 2–7, in the heater's order
BAND_HOURS_KEYS = (
    "hours_power_0_25",
    "hours_power_25_50",
    "hours_power_50_75",
    "hours_power_75_100",
    "hours_control_pause",
    "hours_altitude_mode",
)

DIAGNOSTICS_SCHEMA = cv.Schema(
    {
        # How often the values are read while the heater is starting, running or
        # cooling down. While it idles they are read once a minute.
        cv.Optional(CONF_UPDATE_INTERVAL, default="10s"): cv.All(
            cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(seconds=2))
        ),
        **{cv.Optional(k): schema for k, (schema, _) in DIAG_SENSORS.items()},
        **{cv.Optional(k): schema for k, (schema, _) in DIAG_BINARY_SENSORS.items()},
        **{cv.Optional(k): schema for k, (schema, _) in DIAG_TEXT_SENSORS.items()},
        **{cv.Optional(k): _hours() for k in BAND_HOURS_KEYS},
    }
)

# ── Config schema ─────────────────────────────────────────────────────────
# climate_schema(class) is the modern ESPHome API (replaces CLIMATE_SCHEMA).
# It registers the ID with the correct C++ class automatically.
CONFIG_SCHEMA = (
    climate.climate_schema(EsparCanComponent)
    .extend(
        {
            # Cabin temperature — typically a HA template sensor. Sent to the heater
            # as the room temperature with room_sensor: controller.
            cv.Optional(CONF_CURRENT_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            # Unit of the cabin temperature sensor.
            # "celsius"    (default) — value passed through unchanged.
            # "fahrenheit" — component converts °F→°C internally.  Remove any
            #                lambda filter from the HA sensor block when using this option.
            cv.Optional(CONF_CABIN_TEMP_UNIT, default="celsius"): cv.one_of(
                "celsius", "fahrenheit", lower=True
            ),
            # The room temperature the heater regulates on (#9). Can also be switched
            # at run time with set_room_sensor_controller().
            # "heater" (default) — its own intake sensor, read over UDS (read-only, also
            #                      without a diagnostics: block) minus the heater's 3 K.
            # "controller"       — current_temperature_sensor.
            cv.Optional(CONF_ROOM_SENSOR, default="heater"): cv.one_of(
                "heater", "controller", lower=True
            ),
            # The heater has no run-time limit of its own: HEAT or FAN_ONLY is
            # switched off after this time, however it was started. 0min = no limit.
            # Can be changed at run time with set_max_run_time_minutes().
            cv.Optional(CONF_MAX_RUN_TIME, default="120min"): cv.positive_time_period_minutes,
            # Controller identity. 24-bit ID in 0x60D D3–D5 and the
            # last byte of init frame 0x62. Defaults reproduce the maintainer's Pro.
            cv.Optional(CONF_CONTROLLER_ID, default=0x82B609): cv.int_range(min=0, max=0xFFFFFF),
            cv.Optional(CONF_INIT_0X62_BYTE, default=0xF1): cv.hex_uint8_t,
            # After a power interruption the heater sits in IDLE with D3≠0 and
            # ignores commands. When true, send a FAN→IDLE pulse to release it.
            cv.Optional(CONF_AUTO_CLEAR_IDLE_LOCK, default=True): cv.boolean,
            **{cv.Optional(k): cv.invalid(f"{k}: {_REMOVED}") for k in REMOVED_OPTIONS},
            # Optional entity outputs
            cv.Optional(CONF_HEATER_STATE):     text_sensor.text_sensor_schema(),
            cv.Optional(CONF_FLAME_ACTIVE):     binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_FLAG_0X20): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_HEATER_CONNECTED): binary_sensor.binary_sensor_schema(),
            cv.Optional(CONF_FAULT_TEXT):       text_sensor.text_sensor_schema(),
            # Read-only heater diagnostics over UDS. Off unless this block is present.
            cv.Optional(CONF_DIAGNOSTICS): DIAGNOSTICS_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


# ── Code generation ───────────────────────────────────────────────────────
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await climate.register_climate(var, config)

    cg.add(var.set_cabin_temp_fahrenheit(config[CONF_CABIN_TEMP_UNIT] == "fahrenheit"))
    cg.add(var.set_room_sensor_controller(config[CONF_ROOM_SENSOR] == "controller"))
    cg.add(var.set_max_run_time_minutes(config[CONF_MAX_RUN_TIME].total_minutes))
    cg.add(var.set_controller_id(config[CONF_CONTROLLER_ID]))
    cg.add(var.set_init_0x62_byte(config[CONF_INIT_0X62_BYTE]))
    cg.add(var.set_auto_clear_idle_lock(config[CONF_AUTO_CLEAR_IDLE_LOCK]))

    if CONF_CURRENT_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_CURRENT_TEMPERATURE_SENSOR])
        cg.add(var.set_current_temperature_sensor(sens))

    if CONF_HEATER_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_HEATER_STATE])
        cg.add(var.set_heater_state_sensor(sens))

    if CONF_FLAME_ACTIVE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_FLAME_ACTIVE])
        cg.add(var.set_flame_sensor(sens))

    if CONF_FLAG_0X20 in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_FLAG_0X20])
        cg.add(var.set_flag_0x20_sensor(sens))

    if CONF_HEATER_CONNECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_HEATER_CONNECTED])
        cg.add(var.set_connected_sensor(sens))

    if CONF_FAULT_TEXT in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FAULT_TEXT])
        cg.add(var.set_fault_sensor(sens))

    if CONF_DIAGNOSTICS in config:
        diag = config[CONF_DIAGNOSTICS]
        cg.add(var.set_diagnostics_enabled(True))
        cg.add(var.set_diagnostics_interval(diag[CONF_UPDATE_INTERVAL]))
        for key, (_, setter) in DIAG_SENSORS.items():
            if key in diag:
                sens = await sensor.new_sensor(diag[key])
                cg.add(getattr(var, setter)(sens))
        for key, (_, setter) in DIAG_BINARY_SENSORS.items():
            if key in diag:
                sens = await binary_sensor.new_binary_sensor(diag[key])
                cg.add(getattr(var, setter)(sens))
        for key, (_, setter) in DIAG_TEXT_SENSORS.items():
            if key in diag:
                sens = await text_sensor.new_text_sensor(diag[key])
                cg.add(getattr(var, setter)(sens))
        for band, key in enumerate(BAND_HOURS_KEYS):
            if key in diag:
                sens = await sensor.new_sensor(diag[key])
                cg.add(var.set_band_hours_sensor(band, sens))
