// =============================================================================
//  Espar Airtronic S3 CAN Controller — ESPHome External Component
//  Trooper camper project.  WeAct CAN485 V1.0  (ESP32 TWAI)
// =============================================================================

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "espar_can.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cstring>
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

  // ── RX: process incoming heater frames ──────────────────────────────
  handle_rx_();

  // Capture 'now' AFTER handle_rx_(): note_heater_alive_() sets
  // heater_last_seen_ = millis() while draining RX, so sampling 'now' before
  // that lets (now - heater_last_seen_) underflow (uint32) and spuriously trip
  // the heartbeat timeout — the cause of connect/disconnect flapping.
  uint32_t now = millis();

  // ── Init burst retry until heater acks ──────────────────────────────
  if (!heater_connected_ && (now - t_init_last_) >= INIT_RESEND_MS) {
    send_init_burst_();
    t_init_last_ = now;
    settings_dirty_ = false;
  }

  // ── Room sensor selection changed at run time: tell the heater once ─
  if (settings_dirty_ && heater_connected_) {
    settings_dirty_ = false;
    send_init_burst_();
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

  // ── IDLE lock release (D3≠0 in IDLE after power interruption) ───────
  handle_idle_lock_(now);

  // ── Other controller gone? ───────────────────────────────────────────
  if (foreign_ctrl_present_ && (now - foreign_ctrl_last_ms_) > FOREIGN_CTRL_TIMEOUT_MS) {
    foreign_ctrl_present_ = false;
    ESP_LOGI(TAG, "Other controller no longer seen on bus");
    if (foreign_fault_published_) { foreign_fault_published_ = false; publish_fault_("OK"); }
  }

  // ── UDS diagnostics (only with a `diagnostics:` block) ───────────────
  handle_diagnostics_(now);

  // ── Heartbeat timeout: heater went offline ────────────────────────────
  if (heater_connected_ && (now - heater_last_seen_) > HEARTBEAT_TIMEOUT_MS) {
    heater_connected_ = false;
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

  // ── Keep climate action in sync with actual heater state ──────────────
  update_climate_action_();
}

// =============================================================================
// CLIMATE INTERFACE
// =============================================================================

climate::ClimateTraits EsparCanComponent::traits() {
  auto traits = climate::ClimateTraits();
  // ESPHome 2026.x replaced the set_supports_* booleans with feature flags.
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                           climate::CLIMATE_SUPPORTS_ACTION);
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_visual_min_temperature(10.0f);   // 50°F — heater's own setpoint floor (Airtronic 3 TD §4.4)
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
    apply_climate_mode_();
    this->publish_state();
  }
}

// Register a callback for the cabin temperature (typically an HA API sensor). It is
// sent to the heater as the room temperature when room_sensor is "controller".
void EsparCanComponent::set_current_temperature_sensor(sensor::Sensor *s) {
  current_temp_sensor_ = s;
  s->add_on_state_callback([this](float val) {
    if (!std::isnan(val)) {
      // ESPHome climate always uses °C internally.
      // cabin_temp_fahrenheit_ is set true when cabin_temp_unit: fahrenheit in YAML.
      if (cabin_temp_fahrenheit_)
        val = (val - 32.0f) * 5.0f / 9.0f;
      ext_temp_c_ = val;
      // On the heater's own sensor, the climate entity shows the heater's value
      // instead (publish_room_temperature_()).
      if (needs_intake_()) return;
      this->current_temperature = val;
      this->publish_state();
    }
  });
}

// Climate mode → command. The heater has the real setpoint and decides by itself
// when to burn, modulate or pause, so HEAT keeps the heat request up.
void EsparCanComponent::apply_climate_mode_() {
  if (locked_out_) return;
  switch (this->mode) {
    case climate::CLIMATE_MODE_HEAT:     apply_mode_(true, false);  break;
    case climate::CLIMATE_MODE_FAN_ONLY: apply_mode_(false, true);  break;
    default:                             apply_mode_(false, false); break;
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
    cmd_temp_c_x10_      = setpoint_word_();
    waiting_heat_confirm_ = true;
    ESP_LOGI(TAG, "Commanding HEAT, heater regulates to %.1f°C (CAN word 0x%04X)", cmd_temp_c_x10_ / 10.0f, cmd_temp_c_x10_);
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

  if (cooldown_) {
    // After-run: blower purging the heat exchanger, whatever the mode is
    new_action = climate::CLIMATE_ACTION_FAN;
  } else if (this->mode == climate::CLIMATE_MODE_OFF) {
    new_action = climate::CLIMATE_ACTION_OFF;
  } else if (this->mode == climate::CLIMATE_MODE_FAN_ONLY) {
    new_action = climate::CLIMATE_ACTION_FAN;
  } else if (cmd_heating_) {
    // Report actual heater state so HA can show whether combustion is active.
    // In its own control pause the heater still reports D1=0x09; only FD10 shows it.
    bool pause = diag_phase_ == PHASE_CONTROL_PAUSE &&
                 (millis() - t_diag_phase_) < 3 * diag_interval_ms_;
    new_action = (heater_d1_state_ == HEATER_STATE_HEATING && !pause)
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
    // Rate-limited: with the heater unpowered nothing ACKs our frames and the
    // controller retries endlessly (thousands of errors/s). Logging each one
    // stalls the main loop, so emit one summary per BUS_ERR_LOG_MS instead.
    static constexpr uint32_t BUS_ERR_LOG_MS = 5000;
    uint32_t now = millis();
    if (bus_err_log_ms_ == 0 || (now - bus_err_log_ms_) >= BUS_ERR_LOG_MS) {
      twai_status_info_t st;
      twai_get_status_info(&st);
      uint32_t delta = st.bus_error_count - bus_err_logged_count_;
      ESP_LOGW(TAG, "CAN bus errors: +%lu in last %lus (total %lu)%s",
               (unsigned long) delta, (unsigned long)(BUS_ERR_LOG_MS / 1000),
               (unsigned long) st.bus_error_count,
               heater_connected_ ? "" : " — heater not responding (unpowered?)");
      bus_err_logged_count_ = st.bus_error_count;
      bus_err_log_ms_ = now;
    }
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
          if (!msg.rtr && msg.data_length_code >= 6) {
            note_heater_alive_();   // status frame counts as liveness (variant-robust)
            parse_0x2C4_(msg);
          }
          break;

        case 0x625:
          // Heater heartbeat — also treated as liveness. Kept as an explicit case
          // so the heartbeat alone keeps the connection alive when 0x2C4 is idle.
          note_heater_alive_();
          break;

        case 0x2C5:
        case 0x2C6:
          // Known IDs with no actionable content observed — suppress verbose log
          break;

        case 0x60D:
        case 0x54:
        case 0x55:
        case 0x56:
        case 0x57:
          // Another controller's heartbeat or commands (we never receive our own
          // frames). An EasyStart Pro stops 0x60D ~3–4 min after its last button
          // press but keeps 0x54–0x57 going, so both count as presence.
          note_foreign_controller_(msg);
          [[fallthrough]];
        case 0x65:
        // Init burst sent by a controller at power-up / handshake
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61:
        case 0x62: case 0x63: case 0x64: case 0x66: case 0x67: case 0x68:
        case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x10A:
          // OEM controller command frames (e.g. a real EasyStart Pro sharing the
          // bus). The WeAct emulates these same IDs, so observing them is expected,
          // not a fault. Log at verbose for RE only (~20 frames/s with a Pro on the
          // bus) — do NOT publish a fault or drive the status LED red.
          ESP_LOGV(TAG,
            "OEM controller frame 0x%03lX [%u]: %02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned long)msg.identifier, msg.data_length_code,
            msg.data[0], msg.data[1], msg.data[2], msg.data[3],
            msg.data[4], msg.data[5], msg.data[6], msg.data[7]);
          break;

        case DIAG_RESPONSE_ID:
          // Diagnostic answer (ours, or another tester's while the WeAct is quiet)
          if (diag_active_() && !msg.rtr && msg.data_length_code >= 2) handle_diag_frame_(msg);
          break;

        case DIAG_REQUEST_ID:
          // Another tester's diagnostic request (EasyScan, an EasyStart Pro's workshop menu)
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
  // "Flame" follows the heating state (D1=0x09). D2 bit5 (0x20) was used
  // previously, but a second B2L (near sea level) never sets it, even under the
  // OEM EasyStart Pro, while ours (~2,100 m) sets it ~45 s into each burn.
  // Meaning of 0x20 is unconfirmed (altitude compensation is the leading
  // theory), so it's exposed separately as a diagnostic flag.
  bool     new_flame      = (new_state == HEATER_STATE_HEATING);
  bool     new_flag_0x20  = (new_sub & 0x20) != 0;
  uint16_t new_ctr        = static_cast<uint16_t>(msg.data[4]) |
                            (static_cast<uint16_t>(msg.data[5]) << 8);

  // Log every change of the D3 fault byte (diagnostic; low rate)
  if (new_fault_code != heater_fault_code_) {
    ESP_LOGW(TAG, "D3 fault byte 0x%02X -> 0x%02X (D1=0x%02X D2=0x%02X)",
             heater_fault_code_, new_fault_code, new_state, new_sub);
  }

  // Cooldown / after-run: IDLE with D2 bit3 (0x08) set and no fault code.
  // Seen for ~4 min after heating while the blower purges, then D2 -> 0x00.
  bool new_cooldown = (new_state == HEATER_STATE_IDLE) && (new_sub & 0x08)
                      && (new_fault_code == HEATER_FAULT_NONE);

  // While the heater idles and nothing is commanded, D5/D6 is its own room
  // temperature ((°C + 50) × 10). Once it runs it mirrors what the control unit
  // sends, so the value is only taken here (fallback for own_room_temperature_()).
  if (new_state == HEATER_STATE_IDLE && (new_sub & 0x08) == 0 && !cmd_heating_ && !cmd_fan_ &&
      new_ctr != 0xFFFE && new_ctr != 500) {
    own_room_word_ = new_ctr;
  }

  bool state_changed = (new_cooldown != cooldown_) || (new_state != heater_d1_state_) || (new_flame != flame_active_)
                     || (new_fault_code != heater_fault_code_)
                     || (new_flag_0x20 != flag_0x20_);

  // ── Active fault detection ─────────────────────────────────────────────
  if (new_state == HEATER_STATE_FAULT && heater_d1_state_ != HEATER_STATE_FAULT) {
    // First frame of fault state — command idle and report immediately
    ESP_LOGE(TAG, "HEATER FAULT detected! D1=0x0B D2=0x%02X FaultCode=0x%02X", new_sub, new_fault_code);
    post_fault_window_ = true;
    read_fault_memory();  // fetch the P-code behind it (diagnostics only)
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
      && heater_fault_code_ != HEATER_FAULT_NONE && post_fault_window_) {
    ESP_LOGD(TAG, "Fault code 0x%02X still clearing in IDLE (~26 s window)", new_fault_code);
  }

  // ── IDLE lock: D3≠0 in IDLE without a preceding 0x0B fault ──────────
  // Seen after every heater power interruption (D3=0x04). The heater ignores
  // HEAT/FAN until 0x54 goes non-idle → IDLE. Report it; handle_idle_lock_()
  // releases it.
  if (new_state == HEATER_STATE_IDLE && new_fault_code != HEATER_FAULT_NONE
      && !post_fault_window_ && !idle_lock_) {
    idle_lock_          = true;
    idle_lock_phase_    = 0;
    idle_lock_attempts_ = 0;
    idle_lock_gave_up_  = false;
    idle_lock_next_try_ = millis() + IDLE_LOCK_FIRST_DELAY_MS;
    char buf[64];
    snprintf(buf, sizeof(buf), "IDLE LOCK: D3=0x%02X (heater ignoring commands)", new_fault_code);
    ESP_LOGW(TAG, "%s%s", buf, auto_clear_idle_lock_ ? " — auto-clearing" : "");
    publish_fault_(buf);
  }

  if (new_state == HEATER_STATE_IDLE && new_fault_code == HEATER_FAULT_NONE
      && heater_fault_code_ != HEATER_FAULT_NONE) {
    if (idle_lock_) {
      ESP_LOGI(TAG, "IDLE lock released after %u attempt(s)", (unsigned) idle_lock_attempts_);
    } else {
      ESP_LOGI(TAG, "Fault code cleared — heater returned to normal IDLE");
    }
    idle_lock_         = false;
    idle_lock_phase_   = 0;
    post_fault_window_ = false;
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
    ESP_LOGI(TAG, "Heating confirmed (D1=0x09) — flag 0x20: %s", new_flag_0x20 ? "set" : "clear");
  }

  bool was_active = heater_active_();

  heater_d1_state_ = new_state;
  flame_active_    = new_flame;
  flag_0x20_       = new_flag_0x20;
  cooldown_        = new_cooldown;

  // Read the diagnostics once more right after a run, so the entities end on their
  // resting values instead of the last cooldown sample, and check the fault memory.
  if (was_active && !heater_active_()) read_fault_memory();

  // ── Publish to sensors on state change ────────────────────────────
  if (state_changed) {
    const char *state_str =
      (new_state == HEATER_STATE_HEATING) ? "HEATING" :
      (new_state == HEATER_STATE_FAN)     ? "FAN"     :
      new_cooldown                        ? "COOLDOWN" :
      (new_state == HEATER_STATE_IDLE)    ? "IDLE"    :
      (new_state == HEATER_STATE_STARTUP) ? "STARTUP" :
      (new_state == HEATER_STATE_FAULT)   ? "FAULT"   :
      (new_state == HEATER_STATE_INIT)    ? "INIT"    : "UNKNOWN";

    ESP_LOGI(TAG, "Heater state: %s | Burning: %s | Flag0x20: %s | Echo: %u",
      state_str, new_flame ? "ON" : "off", new_flag_0x20 ? "set" : "clear", new_ctr);
    if (strcmp(state_str, "UNKNOWN") == 0) {
      ESP_LOGW(TAG, "Unmapped 0x2C4 state — raw D1-D4: %02X %02X %02X %02X",
               msg.data[0], msg.data[1], msg.data[2], msg.data[3]);
    }

    if (heater_state_sensor_) heater_state_sensor_->publish_state(state_str);
    if (flame_sensor_)         flame_sensor_->publish_state(new_flame);
    if (flag_0x20_sensor_)     flag_0x20_sensor_->publish_state(new_flag_0x20);
  }
}

// Mark the heater as alive. Called for both the 0x625 heartbeat and 0x2C4 status
// frames, so connection detection survives on variants (e.g. diesel D2L) where the
// 0x625 heartbeat ID or cadence differs from the B2L this was originally built on.
void EsparCanComponent::note_heater_alive_() {
  // Update timestamp FIRST so the timeout check in loop() never sees a stale value
  // even if publish_state() callbacks re-enter loop logic synchronously.
  heater_last_seen_ = millis();
  if (!heater_connected_) {
    heater_connected_   = true;
    failed_start_count_ = 0;   // fresh connection resets failure counter
    locked_out_         = false;
    ESP_LOGI(TAG, "Heater alive (heartbeat/status) — connected");
    if (connected_sensor_) connected_sensor_->publish_state(true);
    if (fault_sensor_)     fault_sensor_->publish_state("OK");
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
// UDS DIAGNOSTICS — read-only, ReadDataByIdentifier (0x22) and
// ReadDTCInformation (0x19) on 0x7A0, answers on 0x73C (ISO-TP)
// =============================================================================
// The heater answers standard UDS requests in every state (idle, fan, heating,
// cooldown). One sweep reads each identifier a configured entity needs, one
// request at a time. Sweeps run every diagnostics update_interval while the
// heater is active and once a minute while it idles. Nothing is sent in CAN
// standby or while the heater is not connected. Nothing is written, reset or
// cleared. Identifiers and scalings: docs/uds-diagnostics.md.

// Whether this item should be requested in the current sweep.
bool EsparCanComponent::diag_wanted_(uint8_t item, uint32_t now) const {
  if (diag_unsupported_ & (1u << item)) return false;
  switch (item) {
    case DIAG_FLAME:        return flame_detected_sensor_ || combustion_state_sensor_;
    case DIAG_TEMPERATURES: return flame_temperature_sensor_ || intake_temperature_sensor_ || needs_intake_();
    case DIAG_VOLTAGE:      return supply_voltage_sensor_ != nullptr;
    case DIAG_FUEL_PUMP:    return fuel_pump_frequency_sensor_ != nullptr;
    case DIAG_GLOW_PLUG:    return glow_plug_sensor_ || glow_plug_power_sensor_;
    case DIAG_PHASE:        return true;  // also shows the control pause in the climate action
    case DIAG_BLOWER:       return blower_level_sensor_ != nullptr;
    case DIAG_FUEL:         return fuel_rate_sensor_ || fuel_total_sensor_;
    case DIAG_POWER_DEMAND: return power_demand_sensor_ != nullptr;
    case DIAG_BURN_TIME:    return burn_time_sensor_ != nullptr;
    case DIAG_REGULATION:   return heater_setpoint_sensor_ || heater_temperature_sensor_;
    case DIAG_ECU_TEMP:     return control_unit_temperature_sensor_ != nullptr;
    case DIAG_PRESSURE:     return air_pressure_sensor_ != nullptr;
    case DIAG_COUNTERS: {
      bool any = operating_hours_sensor_ != nullptr;
      for (auto *s : band_hours_sensor_) any = any || s != nullptr;
      return any && (!diag_hours_read_ || (now - t_diag_hours_last_) >= DIAG_HOURS_INTERVAL_MS);
    }
    case DIAG_STARTS:       return start_stop_count_sensor_ != nullptr;
    case DIAG_PUMP_STROKES: return fuel_pump_strokes_sensor_ != nullptr;
    case DIAG_SW_VERSION:   return software_version_sensor_ && !(diag_read_once_ & (1u << item));
    case DIAG_HW_VERSION:   return hardware_version_sensor_ && !(diag_read_once_ & (1u << item));
    case DIAG_PART_NUMBER:  return part_number_sensor_ && !(diag_read_once_ & (1u << item));
    case DIAG_FAULT_MEMORY: return fault_memory_sensor_ && diag_fault_read_requested_;
    default:                return false;
  }
}

void EsparCanComponent::handle_diagnostics_(uint32_t now) {
  if (!diag_active_()) return;

  if (tx_standby_ || !heater_connected_) {
    // Nothing to ask; start with a full sweep once the heater is back
    diag_waiting_              = false;
    diag_sweep_active_         = false;
    diag_sweep_requested_      = true;
    diag_fault_read_requested_ = true;
    return;
  }

  if (diag_waiting_) {
    if ((now - t_diag_request_) < DIAG_TIMEOUT_MS) return;
    diag_waiting_ = false;
    t_diag_done_  = now;
    ESP_LOGD(TAG, "Diagnostics: no answer to %02X %04X",
             diag_pending_ == DIAG_FAULT_MEMORY ? UDS_READ_DTC : UDS_READ_DATA, DIAG_DIDS[diag_pending_]);
    if (diag_timeouts_ < DIAG_MAX_TIMEOUTS && ++diag_timeouts_ == DIAG_MAX_TIMEOUTS)
      ESP_LOGW(TAG, "Diagnostics: heater does not answer on 0x%03lX — retrying once a minute",
               (unsigned long) DIAG_RESPONSE_ID);
  }

  if (!diag_sweep_active_) {
    bool fast = heater_active_() && diag_timeouts_ < DIAG_MAX_TIMEOUTS;
    uint32_t interval = fast ? diag_interval_ms_ : DIAG_IDLE_INTERVAL_MS;
    if (!diag_sweep_requested_ && (now - t_diag_sweep_last_) < interval) return;
    diag_sweep_requested_ = false;
    diag_sweep_active_    = true;
    diag_sweep_pos_       = 0;
    t_diag_sweep_last_    = now;
  }

  if ((now - t_diag_done_) < DIAG_GAP_MS) return;

  while (diag_sweep_pos_ < DIAG_ITEM_COUNT && !diag_wanted_(diag_sweep_pos_, now))
    diag_sweep_pos_++;
  if (diag_sweep_pos_ >= DIAG_ITEM_COUNT) {
    diag_sweep_active_ = false;
    return;
  }
  diag_send_request_(diag_sweep_pos_++);
}

// ISO-TP single frame: [03][22][DID hi][DID lo]  or  [03][19][02][08], padded with AA
void EsparCanComponent::diag_send_request_(uint8_t item) {
  uint16_t did = DIAG_DIDS[item];
  uint8_t  sid = (item == DIAG_FAULT_MEMORY) ? UDS_READ_DTC : UDS_READ_DATA;
  const uint8_t d[8] = {0x03, sid, static_cast<uint8_t>(did >> 8), static_cast<uint8_t>(did & 0xFF),
                        DIAG_PAD, DIAG_PAD, DIAG_PAD, DIAG_PAD};
  send_frame_(DIAG_REQUEST_ID, d, 8);
  diag_pending_   = item;
  diag_waiting_   = true;
  diag_expected_  = 0;
  diag_len_       = 0;
  t_diag_request_ = millis();
}

// Reassemble the ISO-TP answer (single frame, or first frame + consecutive frames).
void EsparCanComponent::handle_diag_frame_(const twai_message_t &msg) {
  if (!diag_waiting_) return;  // late or unsolicited — e.g. another tester on the bus

  const uint8_t *b   = msg.data;
  uint8_t        dlc = msg.data_length_code;

  switch (b[0] >> 4) {
    case 0x0: {  // single frame
      uint8_t len = b[0] & 0x0F;
      if (len == 0 || len > dlc - 1) return;
      diag_process_response_(b + 1, len);
      break;
    }

    case 0x1: {  // first frame — ask for the rest
      diag_expected_ = (static_cast<uint16_t>(b[0] & 0x0F) << 8) | b[1];
      if (diag_expected_ > DIAG_BUF_SIZE || dlc < 8) {
        ESP_LOGW(TAG, "Diagnostics: answer to %04X too long (%u bytes)", DIAG_DIDS[diag_pending_], diag_expected_);
        diag_waiting_ = false;
        t_diag_done_  = millis();
        return;
      }
      diag_len_ = 6;
      for (int i = 0; i < 6; i++) diag_buf_[i] = b[2 + i];
      diag_next_seq_ = 1;
      // Flow control: send all remaining frames, 20 ms apart
      const uint8_t fc[8] = {0x30, 0x00, 0x14, DIAG_PAD, DIAG_PAD, DIAG_PAD, DIAG_PAD, DIAG_PAD};
      send_frame_(DIAG_REQUEST_ID, fc, 8);
      break;
    }

    case 0x2: {  // consecutive frame
      if (diag_expected_ == 0 || (b[0] & 0x0F) != diag_next_seq_) return;
      diag_next_seq_ = (diag_next_seq_ + 1) & 0x0F;
      for (int i = 1; i < dlc && diag_len_ < diag_expected_; i++) diag_buf_[diag_len_++] = b[i];
      if (diag_len_ >= diag_expected_) diag_process_response_(diag_buf_, diag_expected_);
      break;
    }

    default:
      break;
  }
}

// data = UDS payload: [62][DID hi][DID lo][values...]  or  [59][02][mask][DTCs...]  or  [7F][SID][NRC]
// Bytes after the length announced by ISO-TP are leftovers from the heater's buffer.
void EsparCanComponent::diag_process_response_(const uint8_t *data, uint16_t len) {
  uint16_t did = DIAG_DIDS[diag_pending_];
  uint8_t  sid = (diag_pending_ == DIAG_FAULT_MEMORY) ? UDS_READ_DTC : UDS_READ_DATA;

  if (len >= 3 && data[0] == 0x7F && data[1] == sid) {
    if (data[2] == 0x78) return;  // response pending — keep waiting
    ESP_LOGW(TAG, "Diagnostics: heater rejects %02X %04X (NRC 0x%02X) — not asking again", sid, did, data[2]);
    diag_unsupported_ |= (1u << diag_pending_);
    diag_waiting_  = false;
    diag_timeouts_ = 0;
    t_diag_done_   = millis();
    return;
  }

  // Positive answer = service id + 0x40. A DTC answer echoes the sub-function,
  // then the heater's status availability mask, so only byte 1 is comparable.
  if (len < 3 || data[0] != (sid | 0x40) || data[1] != (did >> 8)) return;
  if (sid == UDS_READ_DATA && (len < 4 || data[2] != (did & 0xFF))) return;

  diag_waiting_  = false;
  diag_timeouts_ = 0;
  t_diag_done_   = millis();

  const uint8_t *v = data + 3;
  uint16_t       n = len - 3;
  auto u16be = [v](uint16_t i) -> uint16_t { return (static_cast<uint16_t>(v[i]) << 8) | v[i + 1]; };
  auto u32be = [v](uint16_t i) -> uint32_t {
    return (static_cast<uint32_t>(v[i]) << 24) | (static_cast<uint32_t>(v[i + 1]) << 16) |
           (static_cast<uint32_t>(v[i + 2]) << 8) | v[i + 3];
  };

  switch (diag_pending_) {
    case DIAG_FLAME:
      // Byte 0: 00 = no flame, 01 = flame (~70 s after the heat command, off 4 s after stop).
      if (flame_detected_sensor_) flame_detected_sensor_->publish_state(v[0] != 0);
      // Byte 1: 01 = no fuel, 02 = fuel pump running (ignition and burning on the Airtronic;
      // the Hydronic S3 also reports 03 while burning).
      if (combustion_state_sensor_ && n >= 2) {
        const char *state = (v[1] == 0x01) ? "No fuel" : (v[1] == 0x02) ? "Fuel on" :
                            (v[1] == 0x03) ? "Burning" : nullptr;
        if (state) {
          combustion_state_sensor_->publish_state(state);
        } else {
          char buf[8];
          snprintf(buf, sizeof(buf), "0x%02X", v[1]);
          combustion_state_sensor_->publish_state(buf);
        }
      }
      break;

    case DIAG_TEMPERATURES:
      // uint16 words, °C = v/100 − 50, FFFF = not fitted.
      // Word 1 = flame sensor, word 2 = intake air.
      if (n >= 2 && flame_temperature_sensor_) {
        uint16_t raw = u16be(0);
        flame_temperature_sensor_->publish_state(raw == 0xFFFF ? NAN : raw / 100.0f - 50.0f);
      }
      if (n >= 4) {
        uint16_t raw = u16be(2);
        intake_temp_c_ = (raw == 0xFFFF) ? NAN : raw / 100.0f - 50.0f;
        t_intake_      = millis();
        if (intake_temperature_sensor_) intake_temperature_sensor_->publish_state(intake_temp_c_);
        if (needs_intake_()) publish_room_temperature_();
      }
      break;

    case DIAG_VOLTAGE:
      if (n >= 2 && supply_voltage_sensor_) supply_voltage_sensor_->publish_state(u16be(0) / 10.0f);
      break;

    case DIAG_FUEL_PUMP:
      // × 0.1 Hz: 1.4 Hz at ignition, 3–4 Hz while burning
      if (fuel_pump_frequency_sensor_) fuel_pump_frequency_sensor_->publish_state(v[0] / 10.0f);
      break;

    case DIAG_GLOW_PLUG:
      // uint16, likely 0.1 W (unit as on the Hydronic S3): 0 = off, ~105–116 W during ignition
      if (n >= 2 && glow_plug_sensor_) glow_plug_sensor_->publish_state(u16be(0) != 0);
      if (n >= 2 && glow_plug_power_sensor_) glow_plug_power_sensor_->publish_state(u16be(0) / 10.0f);
      break;

    case DIAG_PHASE: {
      if (n >= 2) {
        diag_phase_   = v[1];
        t_diag_phase_ = millis();
      }
      // Bytes 2–4 (while starting) or 5–7 (in cooldown): step number and the
      // seconds spent in it × 100. Both groups are zero while burning steadily.
      if (n >= 8) {
        bool in_cooldown = v[5] != 0 || v[6] != 0 || v[7] != 0;
        if (phase_step_sensor_) phase_step_sensor_->publish_state(in_cooldown ? v[5] : v[2]);
        if (phase_time_sensor_) phase_time_sensor_->publish_state(u16be(in_cooldown ? 6 : 3) / 100.0f);
      }
      // Byte 1 = phase. 0x14, 0x1E and 0x28 match the Hydronic S3; the start-up and
      // ignition steps are named after what the other values did while they were active.
      if (n < 2 || !combustion_phase_sensor_) break;
      const char *name =
        (v[1] == 0x00) ? "Off" :
        (v[1] == 0x04 || v[1] == 0x0F || v[1] == 0x10) ? "Startup" :  // ~5 s each, before the glow plug
        (v[1] == 0x07) ? "Pre-glow" :       // glow plug on, fuel pump still off
        (v[1] == 0x08) ? "Ignition" :       // fuel pump starts
        (v[1] == 0x09) ? "Flame ramp-up" :  // flame detected, glow plug turns off
        (v[1] == 0x14) ? "Burning" :
        (v[1] == 0x1E) ? "Cooldown" :
        (v[1] == 0x28) ? "Control pause" :  // heat requested, setpoint reached: no flame (0x2C4 still says HEATING)
        (v[1] == 0xD2) ? "Ventilation" : nullptr;
      if (name) {
        combustion_phase_sensor_->publish_state(name);
      } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%02X", v[1]);
        combustion_phase_sensor_->publish_state(buf);
      }
      break;
    }

    case DIAG_BLOWER:
      // uint16 with an offset of 20000, unit unknown: 0 = stopped, ~1100 fan only, up to ~4560 heating
      if (n >= 2 && blower_level_sensor_) {
        uint16_t raw = u16be(0);
        blower_level_sensor_->publish_state(raw > BLOWER_LEVEL_OFFSET ? raw - BLOWER_LEVEL_OFFSET : 0);
      }
      break;

    case DIAG_FUEL:
      // Byte 1 = fuel rate in µl/s (72 at 4.0 Hz = 0.26 l/h, the B2L's rated maximum).
      // Bytes 2–5 = total fuel in whole litres, bytes 6–9 = nanolitres of the current litre.
      if (fuel_rate_sensor_ && n >= 2) fuel_rate_sensor_->publish_state(v[1] * 3.6f / 1000.0f);
      if (fuel_total_sensor_ && n >= 10) fuel_total_sensor_->publish_state(u32be(2) + u32be(6) / 1.0e9f);
      break;

    case DIAG_POWER_DEMAND:
      // uint16 × 0.01 %: 0 outside steady burning, 10000 at full power
      if (n >= 2 && power_demand_sensor_) power_demand_sensor_->publish_state(u16be(0) / 100.0f);
      break;

    case DIAG_BURN_TIME:
      // uint16 × 0.01 s since the flame was detected; keeps its last value after the stop
      if (n >= 2 && burn_time_sensor_) burn_time_sensor_->publish_state(u16be(0) / 100.0f);
      break;

    case DIAG_REGULATION:
      // Two uint16, both (°C + 50) × 10: the setpoint the heater works with (500 = none)
      // and the temperature it regulates on (the same value as 0x2C4 D5/D6).
      if (n >= 4) {
        uint16_t set = u16be(0);
        if (heater_setpoint_sensor_) heater_setpoint_sensor_->publish_state(set == 500 ? NAN : set / 10.0f - 50.0f);
        if (heater_temperature_sensor_) heater_temperature_sensor_->publish_state(u16be(2) / 10.0f - 50.0f);
      }
      break;

    case DIAG_ECU_TEMP:
      // Same scaling as FD01. Likely the control unit's temperature (unconfirmed).
      if (n >= 2 && control_unit_temperature_sensor_)
        control_unit_temperature_sensor_->publish_state(u16be(0) / 100.0f - 50.0f);
      break;

    case DIAG_PRESSURE:
      // uint16 × 0.1 hPa. Likely the sensor used for altitude compensation (unconfirmed).
      if (n >= 2 && air_pressure_sensor_) air_pressure_sensor_->publish_state(u16be(0) / 10.0f);
      break;

    case DIAG_COUNTERS:
      // Seven uint32, all in minutes. Word 1 counts while the heater is running (fan,
      // heating, cooldown). Words 2–7: time at 0–25 / 25–50 / 50–75 / 75–100 % power,
      // in control pause and in altitude mode.
      if (n >= 4) {
        if (operating_hours_sensor_) operating_hours_sensor_->publish_state(u32be(0) / 60.0f);
        for (uint8_t band = 0; band < 6; band++) {
          if (band_hours_sensor_[band] && n >= 8 + band * 4)
            band_hours_sensor_[band]->publish_state(u32be(4 + band * 4) / 60.0f);
        }
        diag_hours_read_   = true;
        t_diag_hours_last_ = millis();
      }
      break;

    case DIAG_STARTS:
      // uint16, +1 at every start and every burner stop
      if (n >= 2 && start_stop_count_sensor_) start_stop_count_sensor_->publish_state(u16be(0));
      break;

    case DIAG_PUMP_STROKES:
      // uint32, lifetime fuel pump strokes (~18 µl each on the B2L)
      if (n >= 4 && fuel_pump_strokes_sensor_) fuel_pump_strokes_sensor_->publish_state(u32be(0));
      break;

    case DIAG_SW_VERSION:
      if (software_version_sensor_) {
        software_version_sensor_->publish_state(std::string(reinterpret_cast<const char *>(v), n));
        diag_read_once_ |= (1u << diag_pending_);
      }
      break;

    case DIAG_HW_VERSION:
      if (hardware_version_sensor_) {
        hardware_version_sensor_->publish_state(std::string(reinterpret_cast<const char *>(v), n));
        diag_read_once_ |= (1u << diag_pending_);
      }
      break;

    case DIAG_PART_NUMBER:
      // Six BCD bytes (20 20 29 01 50 01 → 20.2029.01.5001) and a two-character index
      if (n >= 6 && part_number_sensor_) {
        char buf[24];
        int pos = snprintf(buf, sizeof(buf), "%02X.%02X%02X.%02X.%02X%02X", v[0], v[1], v[2], v[3], v[4], v[5]);
        if (n >= 8) snprintf(buf + pos, sizeof(buf) - pos, " %c%c", v[6], v[7]);
        part_number_sensor_->publish_state(buf);
        diag_read_once_ |= (1u << diag_pending_);
      }
      break;

    case DIAG_FAULT_MEMORY: {
      // 4 bytes per entry: 3 bytes code (00 01 15 → P000115), 1 byte status (bit0 = active now)
      diag_fault_read_requested_ = false;
      if (!fault_memory_sensor_) break;
      std::string list;
      for (uint16_t i = 0; i + 4 <= n; i += 4) {
        char buf[20];
        snprintf(buf, sizeof(buf), "P%02X%02X%02X%s", v[i], v[i + 1], v[i + 2], (v[i + 3] & 0x01) ? " (active)" : "");
        if (!list.empty()) list += ", ";
        list += buf;
      }
      ESP_LOGI(TAG, "Fault memory: %s", list.empty() ? "empty" : list.c_str());
      fault_memory_sensor_->publish_state(list.empty() ? "None" : list);
      break;
    }

    default:
      break;
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
// Suspending TX lets a real EasyStart Pro / EasyScan share the bus: the WeAct
// emulates the Pro (same CAN IDs); both transmitting has been seen to trigger
// fault P000342 ("invalid configuration — check ADR coding").
// Resuming triggers a fresh init burst to re-establish the handshake.
void EsparCanComponent::set_tx_standby(bool s) {
  if (s == tx_standby_) return;
  tx_standby_ = s;
  if (s) {
    ESP_LOGI(TAG, "CAN TX standby ON — all outbound frames suspended");
    if (foreign_fault_published_) { foreign_fault_published_ = false; publish_fault_("OK"); }
    // Drop command state so we re-sync cleanly on resume
    cmd_heating_      = false;
    cmd_fan_          = false;
  } else {
    ESP_LOGI(TAG, "CAN TX standby OFF — re-running init burst");
    // Re-arm other-controller detection: if a controller was seen during
    // standby and is still talking, its next frame raises the fault now that
    // the WeAct is transmitting again.
    foreign_ctrl_present_ = false;
    restart_handshake_();
  }
}

// Re-establish connection with the heater (fresh init burst on next loop).
void EsparCanComponent::restart_handshake_() {
  heater_connected_ = false;
  init_done_        = false;
  t_init_last_      = millis() - INIT_RESEND_MS;  // fire immediately next loop
}

// Controller identity. See docs/protocol-reference.md (0x60D).
void EsparCanComponent::set_controller_id(uint32_t id24) {
  uint8_t a = (id24 >> 16) & 0xFF, b = (id24 >> 8) & 0xFF, c = id24 & 0xFF;
  if (a == ctrl_id_[0] && b == ctrl_id_[1] && c == ctrl_id_[2]) return;
  ctrl_id_[0] = a; ctrl_id_[1] = b; ctrl_id_[2] = c;
  ESP_LOGW(TAG, "Controller ID (0x60D D3-D5) now %02X %02X %02X — re-running init", a, b, c);
  if (driver_installed_ && !tx_standby_) restart_handshake_();
}

void EsparCanComponent::set_init_0x62_byte(uint8_t v) {
  if (v == init_62_last_) return;
  init_62_last_ = v;
  ESP_LOGW(TAG, "Init frame 0x62 D8 now 0x%02X — re-running init", v);
  if (driver_installed_ && !tx_standby_) restart_handshake_();
}

// Init burst — sent at startup and repeated every 150ms until heater responds.
// Sequence and payloads confirmed from Phase 1B RE capture.
void EsparCanComponent::send_init_burst_() {
  static const uint8_t d5C[] = {0x1E,0x00,0x1E,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t d5D[] = {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0xFF};
  static const uint8_t d5E[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  const uint8_t d62[] = {0x12,0x00,0x00,0x00,0x00,0x00,0x00,init_62_last_};
  // 0x63: D1 = temperature offset ((raw − 50) / 10 °C), D2 = sensor selection
  // (01 = sensor in the control unit), D4 = change counter, as the OEM control unit
  // sends it when a setting changes. D2 stays 01: the room value always comes from
  // the control unit, also when it is the heater's own intake sensor handed back —
  // "no value" (FE FF) with D2 = 00 made the heater regulate on 0 °C.
  const uint8_t d63[] = {0x32, 0x01, 0x01, settings_change_ctr_, 0x00, 0x00, 0x00, 0x00};
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
  const uint8_t d[] = {0x0D,0x10,ctrl_id_[0],ctrl_id_[1],ctrl_id_[2],0x20,0x70,0x00};
  send_frame_(0x60D, d, 8);
}

// Primary command group — sent every 200ms
// 0x54 payload encodes mode and setpoint; 0x55/56/57 are static config
void EsparCanComponent::send_command_group_() {
  uint16_t room  = room_word_();
  uint8_t ctr_lo = static_cast<uint8_t>(room & 0xFF);
  uint8_t ctr_hi = static_cast<uint8_t>((room >> 8) & 0xFF);
  // A new target on the climate entity reaches the heater with the next frame
  if (cmd_heating_) cmd_temp_c_x10_ = setpoint_word_();

  uint8_t d54[8] = {};

  if (idle_lock_phase_ == 1) {
    // IDLE-lock release pulse: FAN (heater ignores it while locked)
    d54[0] = 0x01; d54[1] = 0x02; d54[2] = 0xFE; d54[3] = 0xFF;
  } else if (idle_lock_phase_ == 2) {
    // …then back to IDLE — the non-idle → IDLE edge releases the lock
    d54[0] = 0x00; d54[1] = 0x00; d54[2] = 0xFE; d54[3] = 0xFF;
  } else if (cmd_heating_ && !locked_out_) {
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

// ── Regulation (the heater regulates) ────────────────────────────────────
void EsparCanComponent::set_room_sensor_controller(bool f) {
  if (f == room_sensor_controller_) return;
  room_sensor_controller_ = f;
  if (!driver_installed_) return;
  if (f && current_temp_sensor_ == nullptr)
    ESP_LOGW(TAG, "Room sensor → controller requested, but no current_temperature_sensor: staying on the heater's sensor");
  else
    ESP_LOGI(TAG, "Room sensor → %s", f ? "controller" : "heater");
  settings_change_ctr_ = settings_change_ctr_ % 3 + 1;
  settings_dirty_      = true;
  diag_sweep_requested_ = true;
  if (needs_intake_()) {
    publish_room_temperature_();
  } else if (!std::isnan(ext_temp_c_)) {
    this->current_temperature = ext_temp_c_;
    this->publish_state();
  }
}

// Setpoint sent with a HEAT command: the climate target.
uint16_t EsparCanComponent::setpoint_word_() const {
  float t = std::isnan(this->target_temperature) ? 21.0f : this->target_temperature;
  if (t < 5.0f) t = 5.0f;
  if (t > 35.0f) t = 35.0f;
  return static_cast<uint16_t>(t * 10.0f + 0.5f);
}

// The heater's own room temperature. It can't simply be told to use it — FE FF in the
// room value makes it regulate on 0 °C — so the intake sensor is read over UDS and
// handed back with the heater's own 3 K correction. Fallback: the value it reported
// itself while idle.
float EsparCanComponent::own_room_temperature_() const {
  if (!std::isnan(intake_temp_c_) && (millis() - t_intake_) < INTAKE_MAX_AGE_MS)
    return intake_temp_c_ - INTAKE_TO_ROOM_OFFSET_K;
  if (own_room_word_ != 0xFFFE) return own_room_word_ / 10.0f - 50.0f;
  return NAN;
}

// On the heater's own sensor, the climate entity shows that value.
void EsparCanComponent::publish_room_temperature_() {
  float t = own_room_temperature_();
  if (std::isnan(t)) return;
  if (std::isnan(this->current_temperature) || std::fabs(t - this->current_temperature) >= 0.1f) {
    this->current_temperature = t;
    this->publish_state();
  }
}

// Room temperature the control unit reports in 0x54 D5/D6 and 0x65 D1/D2, (°C + 50) × 10:
// current_temperature_sensor, or the heater's own sensor.
uint16_t EsparCanComponent::room_word_() const {
  float t = room_from_controller_() ? ext_temp_c_ : own_room_temperature_();
  // Without any reading, report the setpoint itself: no heat demand rather than 0 °C
  if (std::isnan(t)) t = std::isnan(this->target_temperature) ? 21.0f : this->target_temperature;
  if (t < -40.0f) t = -40.0f;
  if (t > 80.0f) t = 80.0f;
  return static_cast<uint16_t>((t + 50.0f) * 10.0f + 0.5f);
}

// IDLE-lock release state machine. Pulses FAN for IDLE_LOCK_PULSE_MS, forces
// IDLE for IDLE_LOCK_SETTLE_MS, and retries up to IDLE_LOCK_MAX_TRIES times.
// The user's HEAT/FAN command resumes automatically once the lock clears.
void EsparCanComponent::handle_idle_lock_(uint32_t now) {
  if (!idle_lock_ || !auto_clear_idle_lock_ || tx_standby_ || !heater_connected_) {
    idle_lock_phase_ = 0;
    return;
  }
  switch (idle_lock_phase_) {
    case 0:
      if (idle_lock_attempts_ >= IDLE_LOCK_MAX_TRIES) {
        if (!idle_lock_gave_up_) {
          idle_lock_gave_up_ = true;
          ESP_LOGE(TAG, "IDLE lock not released after %u attempts — set mode Off then On, or power-cycle the heater",
                   (unsigned) idle_lock_attempts_);
          publish_fault_("IDLE LOCK: auto-clear failed — toggle Off/On or power-cycle heater");
        }
        return;
      }
      if (static_cast<int32_t>(now - idle_lock_next_try_) >= 0) {
        idle_lock_attempts_++;
        idle_lock_phase_ = 1;
        idle_lock_t_     = now;
        ESP_LOGW(TAG, "IDLE lock: release attempt %u/%u (FAN pulse → IDLE)",
                 (unsigned) idle_lock_attempts_, (unsigned) IDLE_LOCK_MAX_TRIES);
      }
      break;
    case 1:
      if (now - idle_lock_t_ >= IDLE_LOCK_PULSE_MS) { idle_lock_phase_ = 2; idle_lock_t_ = now; }
      break;
    case 2:
      if (now - idle_lock_t_ >= IDLE_LOCK_SETTLE_MS) {
        idle_lock_phase_    = 0;
        idle_lock_next_try_ = now + IDLE_LOCK_RETRY_MS;
      }
      break;
  }
}

// Another controller's frames (0x60D / 0x54–0x57) seen on the bus.
void EsparCanComponent::note_foreign_controller_(const twai_message_t &msg) {
  foreign_ctrl_last_ms_ = millis();
  // ID only comes from 0x60D. D3 bit 0 toggles on OEM controllers; mask it so the logged ID is stable
  if (msg.identifier == 0x60D) {
    foreign_ctrl_id_[0] = msg.data[2] & 0xFE; foreign_ctrl_id_[1] = msg.data[3]; foreign_ctrl_id_[2] = msg.data[4];
  }
  if (foreign_ctrl_present_) return;
  foreign_ctrl_present_ = true;
  if (tx_standby_) {
    ESP_LOGI(TAG, "Other controller on bus (first frame 0x%03lX; 0x60D ID %02X %02X %02X if seen) — WeAct in CAN standby", (unsigned long) msg.identifier,
             foreign_ctrl_id_[0], foreign_ctrl_id_[1], foreign_ctrl_id_[2]);
  } else {
    ESP_LOGW(TAG, "Other controller on bus (first frame 0x%03lX; 0x60D ID %02X %02X %02X if seen) while WeAct is transmitting — enable CAN Standby", (unsigned long) msg.identifier,
             foreign_ctrl_id_[0], foreign_ctrl_id_[1], foreign_ctrl_id_[2]);
    publish_fault_("OTHER CONTROLLER ON BUS — enable CAN Standby");
    foreign_fault_published_ = true;
  }
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
    uint16_t room  = room_word_();
    uint8_t ctr_lo = static_cast<uint8_t>(room & 0xFF);
    uint8_t ctr_hi = static_cast<uint8_t>((room >> 8) & 0xFF);
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
  ESP_LOGCONFIG(TAG, "  Cabin temp unit: %s", cabin_temp_fahrenheit_ ? "fahrenheit (converting °F→°C)" : "celsius (passthrough)");
  ESP_LOGCONFIG(TAG, "  Controller ID (0x60D D3-D5): %02X %02X %02X | 0x62 D8: 0x%02X",
                ctrl_id_[0], ctrl_id_[1], ctrl_id_[2], init_62_last_);
  ESP_LOGCONFIG(TAG, "  Auto-clear IDLE lock: %s", auto_clear_idle_lock_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Regulation: heater, on %s", room_from_controller_() ? "current_temperature_sensor" : "its own sensor");
  LOG_TEXT_SENSOR("  ",    "Heater State",     heater_state_sensor_);
  LOG_BINARY_SENSOR("  ",  "Heating",          flame_sensor_);
  LOG_BINARY_SENSOR("  ",  "Status Flag 0x20", flag_0x20_sensor_);
  LOG_BINARY_SENSOR("  ",  "Heater Connected", connected_sensor_);
  LOG_TEXT_SENSOR("  ",    "Fault",            fault_sensor_);
  if (diag_enabled_) {
    ESP_LOGCONFIG(TAG, "  Diagnostics (UDS 0x%03lX → 0x%03lX, read-only): every %lus while active, %lus while idle",
                  (unsigned long) DIAG_REQUEST_ID, (unsigned long) DIAG_RESPONSE_ID,
                  (unsigned long)(diag_interval_ms_ / 1000), (unsigned long)(DIAG_IDLE_INTERVAL_MS / 1000));
    LOG_BINARY_SENSOR("  ", "Flame Detected",      flame_detected_sensor_);
    LOG_TEXT_SENSOR("  ",   "Combustion Phase",    combustion_phase_sensor_);
    LOG_SENSOR("  ",        "Flame Temperature",   flame_temperature_sensor_);
    LOG_SENSOR("  ",        "Supply Voltage",      supply_voltage_sensor_);
    LOG_SENSOR("  ",        "Fuel Pump Frequency", fuel_pump_frequency_sensor_);
    LOG_TEXT_SENSOR("  ",   "Fault Memory",        fault_memory_sensor_);
  } else {
    ESP_LOGCONFIG(TAG, "  Diagnostics: off");
  }
}

}  // namespace espar_can
}  // namespace esphome
