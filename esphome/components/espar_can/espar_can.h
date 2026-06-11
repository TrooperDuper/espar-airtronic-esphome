#pragma once
// =============================================================================
//  Espar Airtronic S3 CAN Controller — ESPHome External Component
//  Trooper camper project.  WeAct CAN485 V1.0  (ESP32 TWAI)
//
//  CAN pins: TX=GPIO27  RX=GPIO26  500 kbps  standard 11-bit frames
//
//  Protocol summary (from RE captures):
//    HEATER → CONTROLLER
//      0x625  ~100ms   Heater heartbeat
//      0x2C4  ~220ms   Primary status:  D1=state, D2b5=flame, D5/D6=counter LE
//      0x2C5  ~220ms   Secondary status (all-zero observed)
//      0x2C6  ~220ms   Data  (32 00 7C 01 ... static observed)
//
//    CONTROLLER → HEATER  (we transmit)
//      0x54   200ms   Command  IDLE:    [00][00][FE][FF][ctr_lo][ctr_hi][00][00]
//                              FAN:     [01][02][FE][FF][ctr_lo][ctr_hi][00][00]
//                              HEAT:    [01][05][tmp_lo][tmp_hi][ctr_lo][ctr_hi][00][00]
//                     temp = round((°F-32)*50/9) as uint16 LE
//      0x55   200ms   Config A  10 27 00 00 00 00 00 00
//      0x56   200ms   Config B  10 27 00 00 00 00 00 00
//      0x57   200ms   Config C  00 00 FE FF FE FF 00 00
//      0x60D  100ms   Heartbeat 0D 10 82 B6 09 20 70 00
//      0x65   ~10s    Status burst — 4 frames, D3 cycles 01→02→03
//      0x5C–0x10A     Init burst (sent at startup until 0x625 received)
//
//  Climate entity:
//    Mode HEAT     → thermostat: send HEAT:heat_setpoint_f when below target,
//                                send IDLE when above target (+hysteresis)
//    Mode FAN_ONLY → always send FAN command
//    Mode OFF      → always send IDLE command
//
//  Fault detection (0x2C4 D1=0x0B + D3 fault code — confirmed via RE captures):
//    - D1=0x0B = FAULT state; D3 holds fault code bits
//    - D3=0x20 (bit5) = flame loss / fuel starvation
//    - D3=0x40 (bit6) = overtemperature / exhaust blocked
//    - D3 persists non-zero in IDLE for ~26 s after fault clears
//    - Behavioral: heartbeat loss, startup timeout, failed start counter/lockout
// =============================================================================

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "driver/twai.h"

namespace esphome {
namespace espar_can {

// ── Heater 0x2C4 D1 state bytes ─────────────────────────────────────────
static constexpr uint8_t HEATER_STATE_STARTUP = 0x02;
static constexpr uint8_t HEATER_STATE_IDLE    = 0x03;
static constexpr uint8_t HEATER_STATE_INIT    = 0x08;   // brief transition (1-2 frames)
static constexpr uint8_t HEATER_STATE_HEATING = 0x09;
static constexpr uint8_t HEATER_STATE_FAULT   = 0x0B;   // confirmed via RE captures
static constexpr uint8_t HEATER_STATE_FAN     = 0x21;

// ── D3 fault code bits (0x2C4 byte index 2) ─────────────────────────────
static constexpr uint8_t HEATER_FAULT_NONE     = 0x00;
static constexpr uint8_t HEATER_FAULT_FLAME    = 0x20;  // flame loss / fuel starvation
static constexpr uint8_t HEATER_FAULT_OVERTEMP = 0x40;  // overtemperature / exhaust blocked

// ── Timing (ms) ──────────────────────────────────────────────────────────
static constexpr uint32_t INTERVAL_HEARTBEAT_MS  =   100;
static constexpr uint32_t INTERVAL_CMD_MS        =   200;
static constexpr uint32_t INTERVAL_STATUS_MS     = 10000;
static constexpr uint32_t INIT_RESEND_MS         =   150;
static constexpr uint32_t STATUS_BURST_INTV_MS   =   150;
static constexpr uint8_t  STATUS_BURST_COUNT      =     4;

// ── Safety thresholds ────────────────────────────────────────────────────
static constexpr uint32_t HEARTBEAT_TIMEOUT_MS  = 10000;  // loss of 0x625 (heater occasionally gaps >5s)
static constexpr uint32_t STARTUP_TIMEOUT_MS    = 90000;  // stuck in STARTUP state
static constexpr uint32_t HEAT_CONFIRM_MS       = 30000;  // HEAT cmd but no HEATING state
static constexpr uint8_t  MAX_FAILED_STARTS     =    10;  // → lockout
static constexpr float    HYSTERESIS_C          =  0.3f;  // thermostat dead-band

// ── GPIO ─────────────────────────────────────────────────────────────────
static constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_27;
static constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_26;


class EsparCanComponent : public climate::Climate, public Component {
 public:
  // ── ESPHome lifecycle ─────────────────────────────────────────────────
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── Climate interface ─────────────────────────────────────────────────
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  // ── Setters called from generated __init__.py code ────────────────────
  void set_heat_setpoint_f(float f)                         { heat_setpoint_f_ = f; }
  void set_current_temperature_sensor(sensor::Sensor *s);
  void set_heater_state_sensor(text_sensor::TextSensor *s)  { heater_state_sensor_ = s; }
  void set_flame_sensor(binary_sensor::BinarySensor *s)     { flame_sensor_ = s; }
  void set_connected_sensor(binary_sensor::BinarySensor *s) { connected_sensor_ = s; }
  void set_fault_sensor(text_sensor::TextSensor *s)         { fault_sensor_ = s; }

 protected:
  // ── TX helpers ────────────────────────────────────────────────────────
  void send_frame_(uint32_t id, const uint8_t *data, uint8_t len, bool ext = false);
  void send_init_burst_();
  void send_heartbeat_();
  void send_command_group_();
  void handle_status_burst_(uint32_t now);

  // ── RX helpers ────────────────────────────────────────────────────────
  void handle_rx_();
  void parse_0x2C4_(const twai_message_t &msg);
  void on_unexpected_id_(const twai_message_t &msg);

  // ── Thermostat logic ─────────────────────────────────────────────────
  void evaluate_thermostat_();
  void apply_mode_(bool heat, bool fan);
  void update_climate_action_();

  // ── Error helpers ─────────────────────────────────────────────────────
  void publish_fault_(const std::string &msg);

  // ── Temp conversion ───────────────────────────────────────────────────
  // Espar CAN temp word: round((°F - 32) * 50 / 9)
  // Verified: 66°F→0x00BD  75°F→0x00EF  78°F→0x0100  80°F→0x010B
  static uint16_t fahrenheit_to_can(float f) {
    return static_cast<uint16_t>((f - 32.0f) * 50.0f / 9.0f + 0.5f);
  }

  // ── Sensors ───────────────────────────────────────────────────────────
  text_sensor::TextSensor    *heater_state_sensor_{nullptr};
  binary_sensor::BinarySensor *flame_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  text_sensor::TextSensor    *fault_sensor_{nullptr};
  sensor::Sensor             *current_temp_sensor_{nullptr};

  // ── Config ────────────────────────────────────────────────────────────
  // Fixed high setpoint (°F) sent to heater hardware when our thermostat
  // decides to heat.  Our logic controls on/off; the heater's internal
  // thermocouple just acts as a safety ceiling.
  float heat_setpoint_f_{85.0f};

  // ── CAN state ────────────────────────────────────────────────────────
  uint16_t heater_ctr_{0xFFFE};        // counter echoed from 0x2C4 D5/D6
  uint8_t  heater_d1_state_{0};
  uint8_t  heater_fault_code_{0};      // 0x2C4 D3: 0x00=ok, 0x20=flame loss, 0x40=overtemp
  bool     flame_active_{false};
  bool     heater_connected_{false};
  uint32_t heater_last_seen_{0};
  bool     driver_installed_{false};

  // ── Command state ─────────────────────────────────────────────────────
  bool     cmd_heating_{false};
  bool     cmd_fan_{false};
  uint16_t cmd_temp_c_x10_{0};    // temp word sent in 0x54 D3/D4

  // ── TX timers ────────────────────────────────────────────────────────
  uint32_t t_heartbeat_last_{0};
  uint32_t t_cmd_last_{0};
  uint32_t t_status_last_{0};
  uint32_t t_init_last_{0};
  bool     init_done_{false};

  // ── 0x65 burst sequencing ─────────────────────────────────────────────
  uint8_t  status_seq_{0};           // cycles 0→1→2, maps to D3 01→02→03
  uint8_t  status_burst_idx_{0};
  uint32_t t_burst_frame_last_{0};
  bool     burst_in_progress_{false};

  // ── Error tracking ────────────────────────────────────────────────────
  uint32_t startup_enter_ms_{0};      // when D1 last entered 0x02
  uint8_t  failed_start_count_{0};
  bool     locked_out_{false};
  bool     waiting_heat_confirm_{false};
  uint32_t heat_cmd_ms_{0};          // when we last transitioned to cmd_heating=true
};

}  // namespace espar_can
}  // namespace esphome
