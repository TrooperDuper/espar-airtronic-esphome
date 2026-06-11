// =============================================================================
//  Espar Airtronic S3 CAN Controller — ESPHome External Component
//  Trooper camper project.  WeAct CAN485 V1.0  (ESP32 TWAI)
// =============================================================================

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "espar_can.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cmath>
#include <string>

namespace esphome {
namespace espar_can {

static const char *const TAG = "espar_can";

// =============================================================================
// SETUP
// =============================================================================

void EsparCanComponent::setup() {
  ESP_LOGCONFIG(TAG, "Initialising Espar CAN controller...");

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g, &t, &f) != ESP_OK) {
    ESP_LOGE(TAG, "TWAI driver install failed — check GPIO27/26 wiring");
    this->mark_failed();
    return;
  }
  if (twai_start() != ESP_OK) {
    ESP_LOGE(TAG, "TWAI start failed");
    this->mark_failed();
    return;
  }

  uint32_t alerts = TWAI_ALERT_RX_DATA    | TWAI_ALERT_ERR_PASS |
                    TWAI_ALERT_BUS_ERROR  | TWAI_ALERT_RX_QUEUE_FULL |
                    TWAI_ALERT_BUS_OFF    | TWAI_ALERT_RECOVERY_IN_PROGRESS;
  twai_reconfigure_alerts(alerts, nullptr);

  driver_installed_ = true;

  // Send init burst immediately — heater expects this before responding
  send_init_burst_();
  init_done_   = true;
  t_init_last_ = millis();

  // Publish safe default climate state
  this->mode               = climate::CLIMATE_MODE_OFF;
  this->action             = climate::CLIMATE_ACTION_OFF;
  this->target_temperature = 21.0f;  // 21°C / 70°F default
  this->current_temperature = NAN;
  this->publish_state();

  ESP_LOGCONFIG(TAG, "Espar CAN controller ready.  Waiting for heater heartbeat...");
}

// =============================================================================
// LOOP  — runs every ESPHome main loop tick
// =============================================================================

void EsparCanComponent::loop() {
  if (!driver_installed_) return;

  uint32_t now = millis();

  // ── RX: process incoming heater frames ──────────────────────────────
  handle_rx_();

  // ── Init burst retry until heater acks ──────────────────────────────
  if (!heater_connected_ && (now - t_init_last_) >= INIT_RESEND_MS) {
    send_init_burst_();
    t_init_last_ = now;
  }

  // ── Heartbeat 0x60D @ 100ms ─────────────────────────────────────────
  if ((now - t_heartbeat_last_) >= INTERVAL_HEARTBEAT_MS) {
    send_heartbeat_();
    t_heartbeat_last_ = now;
  }

  // ── Command group 0x54/55/56/57 @ 200ms ─────────────────────────────
  if ((now - t_cmd_last_) >= INTERVAL_CMD_MS) {
    send_command_group_();
    t_cmd_last_ = now;
  }

  // ── 0x65 status burst @ 10s ─────────────────────────────────────────
  handle_status_burst_(now);

  // ── Heartbeat timeout: heater went offline ────────────────────────────
  if (heater_connected_ && (now - heater_last_seen_) > HEARTBEAT_TIMEOUT_MS) {
    heater_connected_ = false;
    heater_ctr_       = 0xFFFE;
    ESP_LOGW(TAG, "Heater heartbeat lost — waiting for reconnect");
    if (connected_sensor_) connected_sensor_->publish_state(false);
    publish_fault_("Heartbeat lost");
  }

  // ── Behavioral: stuck in STARTUP too long → failed start ─────────────
  if (heater_d1_state_ == HEATER_STATE_STARTUP && startup_enter_ms_ != 0) {
    if ((now - startup_enter_ms_) > STARTUP_TIMEOUT_MS) {
      startup_enter_ms_ = 0;
      failed_start_count_++;
      ESP_LOGW(TAG, "Startup timeout — failed start #%u", failed_start_count_);
      if (failed_start_count_ >= MAX_FAILED_STARTS) {
        locked_out_  = true;
        cmd_heating_ = false;
        cmd_fan_     = false;
        publish_fault_("LOCKOUT: max failed starts — power-cycle heater to reset");
        ESP_LOGE(TAG, "Heater locked out.  Power-cycle to reset.");
      } else {
        publish_fault_("Failed start #" + std::to_string(failed_start_count_));
      }
    }
  }

  // ── Behavioral: HEAT cmd sent but heater not heating after 30s → warn ─
  if (waiting_heat_confirm_ && cmd_heating_) {
    if ((now - heat_cmd_ms_) > HEAT_CONFIRM_MS &&
        heater_d1_state_ != HEATER_STATE_HEATING) {
      waiting_heat_confirm_ = false;
      ESP_LOGW(TAG, "Heat confirm timeout — heater not entering HEATING state");
      publish_fault_("Heat confirm timeout");
    }
  }

  // ── Keep climate action in sync with actual heater state ──────────────
  update_climate_action_();
}

// =============================================================================
// CLIMATE INTERFACE
// =============================================================================

climate::ClimateTraits EsparCanComponent::traits() {
  auto traits = climate::ClimateTraits();
  traits.set_supports_current_temperature(true);
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_supports_action(true);
  traits.set_visual_min_temperature(15.0f);   // 59°F
  traits.set_visual_max_temperature(30.0f);   // 86°F
  traits.set_visual_temperature_step(0.5f);
  return traits;
}

// Called when HA user changes mode or setpoint on the climate entity.
void EsparCanComponent::control(const climate::ClimateCall &call) {
  if (locked_out_) {
    ESP_LOGW(TAG, "Ignoring climate control — heater is locked out");
    return;
  }

  bool changed = false;

  if (call.get_mode().has_value()) {
    this->mode = *call.get_mode();
    changed = true;
    ESP_LOGI(TAG, "Climate mode → %s",
      this->mode == climate::CLIMATE_MODE_HEAT     ? "HEAT" :
      this->mode == climate::CLIMATE_MODE_FAN_ONLY ? "FAN_ONLY" : "OFF");
  }

  if (call.get_target_temperature().has_value()) {
    this->target_temperature = *call.get_target_temperature();
    changed = true;
    ESP_LOGD(TAG, "Target temperature → %.1f°C (%.1f°F)",
      this->target_temperature,
      this->target_temperature * 9.0f / 5.0f + 32.0f);
  }

  if (changed) {
    evaluate_thermostat_();
    this->publish_state();
  }
}

// Register a callback so the component re-evaluates whenever the averaged
// cabin temperature updates (typically via HA API sensor).
void EsparCanComponent::set_current_temperature_sensor(sensor::Sensor *s) {
  current_temp_sensor_ = s;
  s->add_on_state_callback([this](float val) {
    if (!std::isnan(val)) {
      // Convert to °C for the thermostat comparison (ESPHome climate always uses °C internally).
      // cabin_temp_fahrenheit_ is set true when cabin_temp_unit: fahrenheit in YAML.
      if (cabin_temp_fahrenheit_)
        val = (val - 32.0f) * 5.0f / 9.0f;
      this->current_temperature = val;
      evaluate_thermostat_();
      this->publish_state();
    }
  });
}

// Core thermostat decision logic.
// Called on every temperature update and on every mode/setpoint change.
void EsparCanComponent::evaluate_thermostat_() {
  if (locked_out_) return;

  switch (this->mode) {
    case climate::CLIMATE_MODE_OFF:
      apply_mode_(false, false);
      break;

    case climate::CLIMATE_MODE_FAN_ONLY:
      apply_mode_(false, true);
      break;

    case climate::CLIMATE_MODE_HEAT: {
      if (std::isnan(this->current_temperature)) {
        // No temperature reading yet — default to heating so the cabin warms up.
        // The heater's internal thermocouple acts as safety ceiling.
        apply_mode_(true, false);
        break;
      }
      float cur = this->current_temperature;
      float tgt = this->target_temperature;
      if (cur < (tgt - HYSTERESIS_C)) {
        // Below setpoint minus hysteresis → start heating
        apply_mode_(true, false);
      } else if (cur >= tgt) {
        // At or above setpoint → pause or idle depending on pause_mode config.
        // pause_mode_fan_=true: stay in FAN_ONLY (blower keeps running, no combustion).
        // pause_mode_fan_=false (default): send IDLE (full ~4-min cooldown cycle).
        apply_mode_(false, pause_mode_fan_);
      }
      // Within hysteresis band → maintain current command state (no change)
      break;
    }

    default:
      break;
  }
}

// Apply a desired command state.  Mutual exclusion is enforced: setting heat
// clears fan and vice versa.  Redundant calls (no state change) are skipped.
void EsparCanComponent::apply_mode_(bool heat, bool fan) {
  // Mutual exclusion
  if (heat) fan = false;
  if (fan)  heat = false;

  if (heat == cmd_heating_ && fan == cmd_fan_) return;  // no change

  cmd_heating_ = heat;
  cmd_fan_     = fan;

  if (heat) {
    cmd_temp_c_x10_      = fahrenheit_to_can(heat_setpoint_f_);
    waiting_heat_confirm_ = true;
    heat_cmd_ms_          = millis();
    ESP_LOGI(TAG, "Commanding HEAT at %.0f°F (CAN word 0x%04X)", heat_setpoint_f_, cmd_temp_c_x10_);
  } else if (fan) {
    waiting_heat_confirm_ = false;
    ESP_LOGI(TAG, "Commanding FAN");
  } else {
    waiting_heat_confirm_ = false;
    ESP_LOGI(TAG, "Commanding IDLE");
  }
}

// Keep the HA climate action indicator in sync with what the heater is actually doing.
void EsparCanComponent::update_climate_action_() {
  climate::ClimateAction new_action;

  if (this->mode == climate::CLIMATE_MODE_OFF) {
    new_action = climate::CLIMATE_ACTION_OFF;
  } else if (this->mode == climate::CLIMATE_MODE_FAN_ONLY) {
    new_action = climate::CLIMATE_ACTION_FAN;
  } else if (cmd_heating_) {
    // Report actual heater state so HA can show whether combustion is active
    new_action = (heater_d1_state_ == HEATER_STATE_HEATING)
        ? climate::CLIMATE_ACTION_HEATING
        : climate::CLIMATE_ACTION_IDLE;
  } else {
    new_action = climate::CLIMATE_ACTION_IDLE;
  }

  if (new_action != this->action) {
    this->action = new_action;
    this->publish_state();
  }
}

// =============================================================================
// RX — RECEIVE HEATER FRAMES
// =============================================================================

void EsparCanComponent::handle_rx_() {
  uint32_t alerts = 0;
  twai_read_alerts(&alerts, 0);  // non-blocking

  if (alerts & TWAI_ALERT_ERR_PASS) {
    twai_status_info_t st;
    twai_get_status_info(&st);
    ESP_LOGW(TAG, "CAN error-passive — bus error count: %lu", (unsigned long)st.bus_error_count);
  }
  if (alerts & TWAI_ALERT_BUS_ERROR) {
    twai_status_info_t st;
    twai_get_status_info(&st);
    ESP_LOGW(TAG, "CAN bus error — count: %lu", (unsigned long)st.bus_error_count);
  }
  if (alerts & TWAI_ALERT_BUS_OFF) {
    // TWAI entered bus-off after 256 un-ACKed TX errors.
    // Initiate recovery so we can resume transmitting once the bus clears.
    // Recovery completes after 128 recessive bits; TWAI auto-restarts.
    ESP_LOGW(TAG, "TWAI bus-off — initiating recovery");
    twai_initiate_recovery();
  }
  if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
    ESP_LOGW(TAG, "CAN RX queue full — frames dropped");
  }

  // Always drain the RX queue regardless of alert flags.
  // In bus-error-heavy environments the TWAI_ALERT_RX_DATA alert can be
  // consumed by an error interrupt before we read it, causing us to miss
  // frames even though they're sitting in the queue.
  {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {

      ESP_LOGVV(TAG, "RX 0x%03lX [%u]: %02X %02X %02X %02X %02X %02X %02X %02X",
        (unsigned long)msg.identifier, msg.data_length_code,
        msg.data[0], msg.data[1], msg.data[2], msg.data[3],
        msg.data[4], msg.data[5], msg.data[6], msg.data[7]);

      switch (msg.identifier) {
        case 0x2C4:
          if (!msg.rtr && msg.data_length_code >= 6)
            parse_0x2C4_(msg);
          break;

        case 0x625:
          // Heater heartbeat — update timestamp FIRST so the timeout check
          // in loop() never sees a stale value even if publish_state() callbacks
          // re-enter loop logic synchronously.
          heater_last_seen_ = millis();
          if (!heater_connected_) {
            heater_connected_   = true;
            failed_start_count_ = 0;   // fresh connection resets failure counter
            locked_out_         = false;
            ESP_LOGI(TAG, "Heater heartbeat received — connected");
            if (connected_sensor_) connected_sensor_->publish_state(true);
            if (fault_sensor_)     fault_sensor_->publish_state("OK");
          }
          break;

        case 0x2C5:
        case 0x2C6:
          // Known IDs with no actionable content observed — suppress verbose log
          break;

        default:
          // Any unknown ID may be a fault-reporting frame from the heater.
          // Log it verbosely so we can RE it later.
          on_unexpected_id_(msg);
          break;
      }
    }
  }
}  // handle_rx_

void EsparCanComponent::parse_0x2C4_(const twai_message_t &msg) {
  uint8_t  new_state      = msg.data[0];
  uint8_t  new_sub        = msg.data[1];
  uint8_t  new_fault_code = msg.data[2];  // D3: fault bits (0x00 = normal)
  bool     new_flame      = (new_sub & 0x20) != 0;
  uint16_t new_ctr        = static_cast<uint16_t>(msg.data[4]) |
                            (static_cast<uint16_t>(msg.data[5]) << 8);

  // Always update counter — used in our TX frames
  heater_ctr_ = new_ctr;

  bool state_changed = (new_state != heater_d1_state_) || (new_flame != flame_active_)
                     || (new_fault_code != heater_fault_code_);

  // ── Active fault detection ─────────────────────────────────────────────
  if (new_state == HEATER_STATE_FAULT && heater_d1_state_ != HEATER_STATE_FAULT) {
    // First frame of fault state — command idle and report immediately
    ESP_LOGE(TAG, "HEATER FAULT detected! D1=0x0B D2=0x%02X FaultCode=0x%02X", new_sub, new_fault_code);
    cmd_heating_ = false;
    cmd_fan_     = false;

    std::string fault_msg = "FAULT";
    if (new_fault_code & HEATER_FAULT_FLAME) {
      fault_msg += ":FLAME_LOSS";
      ESP_LOGE(TAG, "  Fault type: FLAME LOSS / FUEL STARVATION (D3 bit5=0x20)");
      ESP_LOGE(TAG, "  → Check: fuel line not kinked, fuel tank not empty");
    }
    if (new_fault_code & HEATER_FAULT_OVERTEMP) {
      fault_msg += ":OVERTEMP";
      ESP_LOGE(TAG, "  Fault type: OVERTEMPERATURE / EXHAUST BLOCKED (D3 bit6=0x40)");
      ESP_LOGE(TAG, "  → Check: exhaust outlet is clear");
    }
    if (new_fault_code == HEATER_FAULT_NONE) {
      fault_msg += ":UNKNOWN";
      ESP_LOGE(TAG, "  Fault type: unknown (D3=0x00, fault code not populated)");
    }
    publish_fault_(fault_msg);
  }

  // ── Post-fault code still clearing (D1=IDLE but D3 non-zero, ~26 s window) ──
  if (new_state == HEATER_STATE_IDLE && new_fault_code != HEATER_FAULT_NONE
      && heater_fault_code_ != HEATER_FAULT_NONE) {
    ESP_LOGD(TAG, "Fault code 0x%02X still clearing in IDLE (~26 s window)", new_fault_code);
  }
  if (new_state == HEATER_STATE_IDLE && new_fault_code == HEATER_FAULT_NONE
      && heater_fault_code_ != HEATER_FAULT_NONE) {
    ESP_LOGI(TAG, "Fault code cleared — heater returned to normal IDLE");
    publish_fault_("OK");
  }

  heater_fault_code_ = new_fault_code;

  // ── Error tracking: startup state transitions ──────────────────────
  if (new_state == HEATER_STATE_STARTUP && heater_d1_state_ != HEATER_STATE_STARTUP) {
    // Just entered startup — start the watchdog
    startup_enter_ms_ = millis();
    ESP_LOGD(TAG, "Heater entering STARTUP state");
  }

  if (heater_d1_state_ == HEATER_STATE_STARTUP && new_state != HEATER_STATE_STARTUP) {
    // Left startup
    startup_enter_ms_ = 0;
    if (new_state == HEATER_STATE_IDLE && cmd_heating_) {
      // Went STARTUP → IDLE while we wanted HEAT → failed start
      failed_start_count_++;
      ESP_LOGW(TAG, "STARTUP → IDLE without HEATING — failed start #%u", failed_start_count_);
      if (failed_start_count_ >= MAX_FAILED_STARTS) {
        locked_out_  = true;
        cmd_heating_ = false;
        cmd_fan_     = false;
        publish_fault_("LOCKOUT: max failed starts — power-cycle heater to reset");
        ESP_LOGE(TAG, "Heater locked out after %u consecutive failed starts", failed_start_count_);
      } else {
        publish_fault_("Failed start #" + std::to_string(failed_start_count_));
      }
    }
  }

  // ── Confirm heating started ────────────────────────────────────────
  if (new_state == HEATER_STATE_HEATING && waiting_heat_confirm_) {
    waiting_heat_confirm_ = false;
    failed_start_count_   = 0;  // successful start resets the counter
    ESP_LOGI(TAG, "Heating confirmed — flame: %s", new_flame ? "active" : "off");
  }

  heater_d1_state_ = new_state;
  flame_active_    = new_flame;

  // ── Publish to sensors on state change ────────────────────────────
  if (state_changed) {
    const char *state_str =
      (new_state == HEATER_STATE_HEATING) ? "HEATING" :
      (new_state == HEATER_STATE_FAN)     ? "FAN"     :
      (new_state == HEATER_STATE_IDLE)    ? "IDLE"    :
      (new_state == HEATER_STATE_STARTUP) ? "STARTUP" :
      (new_state == HEATER_STATE_FAULT)   ? "FAULT"   :
      (new_state == HEATER_STATE_INIT)    ? "INIT"    : "UNKNOWN";

    ESP_LOGI(TAG, "Heater state: %s | Flame: %s | Ctr: 0x%04X",
      state_str, new_flame ? "ON" : "off", new_ctr);

    if (heater_state_sensor_) heater_state_sensor_->publish_state(state_str);
    if (flame_sensor_)         flame_sensor_->publish_state(new_flame);
  }
}

// Log unexpected CAN IDs for future RE analysis.
// Note: fault states are encoded in 0x2C4 D1=0x0B + D3 (not a separate frame ID).
void EsparCanComponent::on_unexpected_id_(const twai_message_t &msg) {
  ESP_LOGD(TAG,
    "Unexpected CAN ID 0x%03lX [%u]: %02X %02X %02X %02X %02X %02X %02X %02X",
    (unsigned long)msg.identifier, msg.data_length_code,
    msg.data[0], msg.data[1], msg.data[2], msg.data[3],
    msg.data[4], msg.data[5], msg.data[6], msg.data[7]);

  // Publish to fault sensor so HA can capture the raw info
  if (fault_sensor_) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Unknown ID 0x%03lX: %02X %02X %02X %02X",
      (unsigned long)msg.identifier,
      msg.data[0], msg.data[1], msg.data[2], msg.data[3]);
    fault_sensor_->publish_state(buf);
  }
}

// =============================================================================
// TX — TRANSMIT CONTROLLER FRAMES
// =============================================================================

void EsparCanComponent::send_frame_(uint32_t id, const uint8_t *data, uint8_t len, bool ext) {
  if (tx_standby_) return;  // CAN TX suspended — OEM tool may be connected
  twai_message_t msg = {};
  msg.identifier       = id;
  msg.extd             = ext ? 1 : 0;
  msg.data_length_code = len;
  for (int i = 0; i < len && i < 8; i++) msg.data[i] = data[i];
  twai_transmit(&msg, pdMS_TO_TICKS(5));
}

// Called from YAML switch lambda (Espar CAN Standby).
// Suspending TX allows OEM EasyStart Pro / EasyScan to connect without
// triggering fault P000342 (too many CAN controllers).
// Resuming triggers a fresh init burst to re-establish the handshake.
void EsparCanComponent::set_tx_standby(bool s) {
  if (s == tx_standby_) return;
  tx_standby_ = s;
  if (s) {
    ESP_LOGI(TAG, "CAN TX standby ON — all outbound frames suspended");
    // Drop command state so we re-sync cleanly on resume
    cmd_heating_      = false;
    cmd_fan_          = false;
  } else {
    ESP_LOGI(TAG, "CAN TX standby OFF — re-running init burst");
    // Re-establish connection with the heater
    heater_connected_ = false;
    heater_ctr_       = 0xFFFE;
    init_done_        = false;
    t_init_last_      = millis() - INIT_RESEND_MS;  // fire immediately next loop
  }
}

// Init burst — sent at startup and repeated every 150ms until heater responds.
// Sequence and payloads confirmed from Phase 1B RE capture.
void EsparCanComponent::send_init_burst_() {
  static const uint8_t d5C[] = {0x1E,0x00,0x1E,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t d5D[] = {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0xFF};
  static const uint8_t d5E[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t d62[] = {0x12,0x00,0x00,0x00,0x00,0x00,0x00,0xF1};
  static const uint8_t d63[] = {0x32,0x01,0x01,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t d64[] = {0x3C,0x00,0x05,0x00,0xC8,0x00,0xFF,0x03};
  static const uint8_t d66[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t d67[] = {0xBE,0x3E,0x27,0x00,0x01,0x1E,0x00,0x00};
  static const uint8_t d6D[] = {0x12,0x00,0x00,0x00,0x00,0x00,0x00,0xF0};
  static const uint8_t d10A[]= {0x10,0x00,0x01,0x20,0x10,0x01,0x00,0x00};

  send_frame_(0x5C, d5C, 8);
  send_frame_(0x5F, d5C, 8);   // 0x5F = same payload as 0x5C
  send_frame_(0x62, d62, 8);
  send_frame_(0x63, d63, 8);
  send_frame_(0x66, d66, 8);
  send_frame_(0x6D, d6D, 8);
  send_frame_(0x10A,d10A, 6);  // DLC=6 confirmed from capture
  send_frame_(0x5D, d5D, 8);
  send_frame_(0x5E, d5E, 8);
  send_frame_(0x60, d5D, 8);   // 0x60 = same payload as 0x5D
  send_frame_(0x61, d5E, 8);   // 0x61 = same payload as 0x5E
  send_frame_(0x64, d64, 8);
  send_frame_(0x67, d67, 8);
  send_frame_(0x68, d67, 8);
  send_frame_(0x69, d67, 8);
  send_frame_(0x6A, d67, 8);
  send_frame_(0x6B, d67, 8);
  send_frame_(0x6C, d67, 8);
}

// Controller alive heartbeat — sent every 100ms
void EsparCanComponent::send_heartbeat_() {
  static const uint8_t d[] = {0x0D,0x10,0x82,0xB6,0x09,0x20,0x70,0x00};
  send_frame_(0x60D, d, 8);
}

// Primary command group — sent every 200ms
// 0x54 payload encodes mode and setpoint; 0x55/56/57 are static config
void EsparCanComponent::send_command_group_() {
  uint8_t ctr_lo = static_cast<uint8_t>(heater_ctr_ & 0xFF);
  uint8_t ctr_hi = static_cast<uint8_t>((heater_ctr_ >> 8) & 0xFF);

  uint8_t d54[8] = {};

  if (cmd_heating_ && !locked_out_) {
    // HEAT mode — D2=0x05 (heating), D3/D4 = temperature word LE
    d54[0] = 0x01;
    d54[1] = 0x05;
    d54[2] = static_cast<uint8_t>(cmd_temp_c_x10_ & 0xFF);
    d54[3] = static_cast<uint8_t>((cmd_temp_c_x10_ >> 8) & 0xFF);
  } else if (cmd_fan_ && !locked_out_) {
    // FAN mode — D2=0x02, temp bytes fixed at 0xFE 0xFF
    d54[0] = 0x01;
    d54[1] = 0x02;
    d54[2] = 0xFE;
    d54[3] = 0xFF;
  } else {
    // IDLE — D1=0x00 D2=0x00
    d54[0] = 0x00;
    d54[1] = 0x00;
    d54[2] = 0xFE;
    d54[3] = 0xFF;
  }
  d54[4] = ctr_lo;
  d54[5] = ctr_hi;
  d54[6] = 0x00;
  d54[7] = 0x00;
  send_frame_(0x54, d54, 8);

  static const uint8_t d55[] = {0x10,0x27,0x00,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t d56[] = {0x10,0x27,0x00,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t d57[] = {0x00,0x00,0xFE,0xFF,0xFE,0xFF,0x00,0x00};
  send_frame_(0x55, d55, 8);
  send_frame_(0x56, d56, 8);
  send_frame_(0x57, d57, 8);
}

// 0x65 periodic status burst — 4 frames per group, D3 cycles 01→02→03
// Groups spaced 10s apart; frames within a group spaced 150ms apart
void EsparCanComponent::handle_status_burst_(uint32_t now) {
  if (!burst_in_progress_) {
    if ((now - t_status_last_) >= INTERVAL_STATUS_MS) {
      burst_in_progress_ = true;
      status_burst_idx_  = 0;
      t_burst_frame_last_ = now;
    }
    return;
  }

  if ((now - t_burst_frame_last_) >= STATUS_BURST_INTV_MS || status_burst_idx_ == 0) {
    static const uint8_t d3_vals[] = {0x01, 0x02, 0x03};
    uint8_t ctr_lo = static_cast<uint8_t>(heater_ctr_ & 0xFF);
    uint8_t ctr_hi = static_cast<uint8_t>((heater_ctr_ >> 8) & 0xFF);
    uint8_t d65[8] = {ctr_lo, ctr_hi, d3_vals[status_seq_], 0x00, 0x00, 0x00, 0x00, 0x00};
    send_frame_(0x65, d65, 8);
    t_burst_frame_last_ = now;
    status_burst_idx_++;

    if (status_burst_idx_ >= STATUS_BURST_COUNT) {
      status_seq_        = (status_seq_ + 1) % 3;
      burst_in_progress_ = false;
      t_status_last_     = now;
    }
  }
}

// =============================================================================
// UTILITY
// =============================================================================

void EsparCanComponent::publish_fault_(const std::string &msg) {
  ESP_LOGW(TAG, "Fault: %s", msg.c_str());
  if (fault_sensor_) fault_sensor_->publish_state(msg);
}

void EsparCanComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Espar Airtronic S3 CAN Controller:");
  ESP_LOGCONFIG(TAG, "  CAN TX: GPIO%d  RX: GPIO%d  500 kbps", CAN_TX_PIN, CAN_RX_PIN);
  ESP_LOGCONFIG(TAG, "  Heat setpoint to heater: %.0f°F", heat_setpoint_f_);
  ESP_LOGCONFIG(TAG, "  Cabin temp unit: %s", cabin_temp_fahrenheit_ ? "fahrenheit (converting °F→°C)" : "celsius (passthrough)");
  ESP_LOGCONFIG(TAG, "  Pause mode: %s", pause_mode_fan_ ? "fan (FAN_ONLY at setpoint)" : "off (IDLE at setpoint)");
  ESP_LOGCONFIG(TAG, "  Thermostat hysteresis: %.1f°C", HYSTERESIS_C);
  LOG_TEXT_SENSOR("  ",    "Heater State",     heater_state_sensor_);
  LOG_BINARY_SENSOR("  ",  "Flame Active",     flame_sensor_);
  LOG_BINARY_SENSOR("  ",  "Heater Connected", connected_sensor_);
  LOG_TEXT_SENSOR("  ",    "Fault",            fault_sensor_);
}

}  // namespace espar_can
}  // namespace esphome
