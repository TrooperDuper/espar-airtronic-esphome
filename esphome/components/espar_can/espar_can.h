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
//      0x2C4  ~220ms   Primary status:  D1=state (0x09=heating/burning),
//                      D2b5=flag 0x20 (meaning unconfirmed), D5/D6=temperature echo LE
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
//    DIAGNOSTICS  (optional `diagnostics:` block; read-only, off by default)
//      0x7A0 → 0x73C  UDS ReadDataByIdentifier (0x22) and ReadDTCInformation
//                     (0x19 02 08) over ISO-TP, padding AA. Nothing is written,
//                     reset or cleared. See docs/uds-diagnostics.md.
//
//  Climate entity:
//    Mode HEAT     → send HEAT continuously with the climate target as setpoint
//                    and a room temperature in 0x54 D5/D6; the heater starts,
//                    modulates, pauses and restarts by itself (like the OEM
//                    EasyStart Pro). The component makes no on/off decisions.
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
static constexpr uint8_t  MAX_FAILED_STARTS     =    10;  // → lockout
static constexpr uint32_t FOREIGN_CTRL_TIMEOUT_MS = 5000; // other controller considered gone after this
// Timings mimic the manual sequence that released the lock every time:
// FAN held ≥5 s, then IDLE. Short early pulses (600 ms, ~2 s after power-up)
// failed and left the lock harder to clear, so wait for the heater to settle.
static constexpr uint32_t IDLE_LOCK_FIRST_DELAY_MS = 45000;  // after lock detected (lets HA reconnect first)
static constexpr uint32_t IDLE_LOCK_PULSE_MS    =  6000;  // FAN held
static constexpr uint32_t IDLE_LOCK_SETTLE_MS   =  5000;  // forced IDLE after the pulse
static constexpr uint32_t IDLE_LOCK_RETRY_MS    = 20000;  // gap between attempts
static constexpr uint8_t  IDLE_LOCK_MAX_TRIES   =     3;

// ── UDS diagnostics (ISO 14229 over ISO-TP), read-only ───────────────────
static constexpr uint32_t DIAG_REQUEST_ID        = 0x7A0;  // tester → heater
static constexpr uint32_t DIAG_RESPONSE_ID       = 0x73C;  // heater → tester
static constexpr uint32_t DIAG_TIMEOUT_MS        =   500;  // per request
static constexpr uint32_t DIAG_GAP_MS            =    50;  // pause between requests
static constexpr uint32_t DIAG_IDLE_INTERVAL_MS  = 60000;  // sweep period while the heater idles
static constexpr uint32_t DIAG_HOURS_INTERVAL_MS = 60000;  // FD17 counters move once a minute
static constexpr uint8_t  DIAG_MAX_TIMEOUTS      =    10;  // consecutive → fall back to the idle period
static constexpr uint8_t  DIAG_PAD               =  0xAA;
static constexpr size_t   DIAG_BUF_SIZE          =    64;  // longest polled answer is 31 bytes
static constexpr uint8_t  UDS_READ_DATA          =  0x22;  // ReadDataByIdentifier
static constexpr uint8_t  UDS_READ_DTC           =  0x19;  // ReadDTCInformation
static constexpr uint16_t BLOWER_LEVEL_OFFSET    = 20000;  // FD08 reads 20000 with the blower stopped

// ── Regulation (the heater regulates) ────────────────────────────────────
// The heater's own room temperature (0x2C4 D5/D6 while idle) is its intake air
// sensor (FD01 word 2) minus 3.0 K: 20.5 → 17.5, 15.6 → 12.6, 14.1 → 11.2 °C.
static constexpr float    INTAKE_TO_ROOM_OFFSET_K = 3.0f;
static constexpr uint32_t INTAKE_MAX_AGE_MS       = 120000;  // older readings are not used
static constexpr uint8_t  PHASE_CONTROL_PAUSE     = 0x28;    // FD10 byte 1

// Requests of one sweep, in order (flame first — it is the time-critical one)
enum DiagItem : uint8_t {
  DIAG_FLAME = 0,      // FD0F
  DIAG_TEMPERATURES,   // FD01
  DIAG_VOLTAGE,        // FD04
  DIAG_FUEL_PUMP,      // FD0D
  DIAG_GLOW_PLUG,      // FD0A
  DIAG_PHASE,          // FD10
  DIAG_BLOWER,         // FD08
  DIAG_FUEL,           // FD11
  DIAG_POWER_DEMAND,   // FD0E
  DIAG_BURN_TIME,      // FD2A
  DIAG_REGULATION,     // FD45
  DIAG_ECU_TEMP,       // FD35
  DIAG_PRESSURE,       // FD36
  DIAG_COUNTERS,       // FD17
  DIAG_STARTS,         // FD06
  DIAG_PUMP_STROKES,   // FD6A
  DIAG_SW_VERSION,     // F195
  DIAG_HW_VERSION,     // F193
  DIAG_PART_NUMBER,    // F1F0
  DIAG_FAULT_MEMORY,   // not a data identifier: ReadDTCInformation 19 02 08
  DIAG_ITEM_COUNT,
};
static constexpr uint16_t DIAG_DIDS[DIAG_ITEM_COUNT] = {
  0xFD0F, 0xFD01, 0xFD04, 0xFD0D, 0xFD0A, 0xFD10, 0xFD08, 0xFD11, 0xFD0E, 0xFD2A,
  0xFD45, 0xFD35, 0xFD36, 0xFD17, 0xFD06, 0xFD6A, 0xF195, 0xF193, 0xF1F0,
  0x0208,  // sub-function 02 (by status mask), mask 08 (confirmed) — sent with service 19
};

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
  void set_cabin_temp_fahrenheit(bool f)                    { cabin_temp_fahrenheit_ = f; }
  void set_auto_clear_idle_lock(bool b)                     { auto_clear_idle_lock_ = b; }
  void set_tx_standby(bool s);
  // Which room temperature the heater regulates on. Can be changed at run time,
  // e.g. from a template select:  - lambda: id(espar).set_room_sensor_controller(x == "External");
  //   false — its own sensor (intake air read over UDS, minus the heater's 3 K)
  //   true  — current_temperature_sensor, sent as the control unit's sensor
  void set_room_sensor_controller(bool f);
  // Controller identity: 24-bit ID sent in 0x60D D3–D5, and the
  // last byte of init frame 0x62. Changing either at runtime re-runs the init
  // handshake (same path as leaving CAN standby).
  void set_controller_id(uint32_t id24);
  void set_init_0x62_byte(uint8_t b);
  uint8_t  get_init_0x62_byte() const { return init_62_last_; }
  uint32_t get_controller_id() const {
    return (uint32_t(ctrl_id_[0]) << 16) | (uint32_t(ctrl_id_[1]) << 8) | ctrl_id_[2];
  }
  void set_current_temperature_sensor(sensor::Sensor *s);
  void set_heater_state_sensor(text_sensor::TextSensor *s)  { heater_state_sensor_ = s; }
  void set_flame_sensor(binary_sensor::BinarySensor *s)     { flame_sensor_ = s; }
  void set_flag_0x20_sensor(binary_sensor::BinarySensor *s) { flag_0x20_sensor_ = s; }
  void set_connected_sensor(binary_sensor::BinarySensor *s) { connected_sensor_ = s; }
  void set_fault_sensor(text_sensor::TextSensor *s)         { fault_sensor_ = s; }

  // ── Diagnostics (UDS, read-only) — only active with a `diagnostics:` block ──
  void set_diagnostics_enabled(bool e)                             { diag_enabled_ = e; }
  void set_diagnostics_interval(uint32_t ms)                       { diag_interval_ms_ = ms; }
  void set_flame_detected_sensor(binary_sensor::BinarySensor *s)   { flame_detected_sensor_ = s; }
  void set_combustion_state_sensor(text_sensor::TextSensor *s)     { combustion_state_sensor_ = s; }
  void set_flame_temperature_sensor(sensor::Sensor *s)             { flame_temperature_sensor_ = s; }
  void set_intake_temperature_sensor(sensor::Sensor *s)            { intake_temperature_sensor_ = s; }
  void set_supply_voltage_sensor(sensor::Sensor *s)                { supply_voltage_sensor_ = s; }
  void set_fuel_pump_frequency_sensor(sensor::Sensor *s)           { fuel_pump_frequency_sensor_ = s; }
  void set_glow_plug_sensor(binary_sensor::BinarySensor *s)        { glow_plug_sensor_ = s; }
  void set_glow_plug_power_sensor(sensor::Sensor *s)               { glow_plug_power_sensor_ = s; }
  void set_combustion_phase_sensor(text_sensor::TextSensor *s)     { combustion_phase_sensor_ = s; }
  void set_phase_step_sensor(sensor::Sensor *s)                    { phase_step_sensor_ = s; }
  void set_phase_time_sensor(sensor::Sensor *s)                    { phase_time_sensor_ = s; }
  void set_blower_level_sensor(sensor::Sensor *s)                  { blower_level_sensor_ = s; }
  void set_fuel_rate_sensor(sensor::Sensor *s)                     { fuel_rate_sensor_ = s; }
  void set_fuel_total_sensor(sensor::Sensor *s)                    { fuel_total_sensor_ = s; }
  void set_power_demand_sensor(sensor::Sensor *s)                  { power_demand_sensor_ = s; }
  void set_burn_time_sensor(sensor::Sensor *s)                     { burn_time_sensor_ = s; }
  void set_heater_setpoint_sensor(sensor::Sensor *s)               { heater_setpoint_sensor_ = s; }
  void set_heater_temperature_sensor(sensor::Sensor *s)            { heater_temperature_sensor_ = s; }
  void set_control_unit_temperature_sensor(sensor::Sensor *s)      { control_unit_temperature_sensor_ = s; }
  void set_air_pressure_sensor(sensor::Sensor *s)                  { air_pressure_sensor_ = s; }
  void set_operating_hours_sensor(sensor::Sensor *s)               { operating_hours_sensor_ = s; }
  void set_band_hours_sensor(uint8_t band, sensor::Sensor *s)      { if (band < 6) band_hours_sensor_[band] = s; }
  void set_start_stop_count_sensor(sensor::Sensor *s)              { start_stop_count_sensor_ = s; }
  void set_fuel_pump_strokes_sensor(sensor::Sensor *s)             { fuel_pump_strokes_sensor_ = s; }
  void set_software_version_sensor(text_sensor::TextSensor *s)     { software_version_sensor_ = s; }
  void set_hardware_version_sensor(text_sensor::TextSensor *s)     { hardware_version_sensor_ = s; }
  void set_part_number_sensor(text_sensor::TextSensor *s)          { part_number_sensor_ = s; }
  void set_fault_memory_sensor(text_sensor::TextSensor *s)         { fault_memory_sensor_ = s; }

  // Read the heater's fault memory now (also done on connect, on a fault and after
  // each run). Callable from YAML:  - lambda: id(espar).read_fault_memory();
  void read_fault_memory() { diag_fault_read_requested_ = true; diag_sweep_requested_ = true; }

 protected:
  // ── TX helpers ────────────────────────────────────────────────────────
  void send_frame_(uint32_t id, const uint8_t *data, uint8_t len, bool ext = false);
  void send_init_burst_();
  void send_heartbeat_();
  void send_command_group_();
  void handle_status_burst_(uint32_t now);
  void handle_idle_lock_(uint32_t now);

  // ── RX helpers ────────────────────────────────────────────────────────
  void handle_rx_();
  void parse_0x2C4_(const twai_message_t &msg);
  void on_unexpected_id_(const twai_message_t &msg);
  // Marks the heater alive (latches connection + refreshes liveness timestamp).
  // Called for both the 0x625 heartbeat and 0x2C4 status frames.
  void note_heater_alive_();

  // ── UDS diagnostics ───────────────────────────────────────────────────
  void handle_diagnostics_(uint32_t now);
  bool diag_wanted_(uint8_t item, uint32_t now) const;
  void diag_send_request_(uint8_t item);
  void handle_diag_frame_(const twai_message_t &msg);
  void diag_process_response_(const uint8_t *data, uint16_t len);
  // Starting, heating, fan or cooldown — anything but plain IDLE
  bool heater_active_() const { return heater_d1_state_ != HEATER_STATE_IDLE || cooldown_; }
  // Diagnostics block configured, or the heater's own sensor is needed for regulation
  bool diag_active_() const { return diag_enabled_ || needs_intake_(); }
  bool needs_intake_() const { return !room_from_controller_(); }

  // ── Climate mode → command ───────────────────────────────────────────
  void apply_climate_mode_();
  void apply_mode_(bool heat, bool fan);
  void update_climate_action_();

  // ── Error helpers ─────────────────────────────────────────────────────
  void publish_fault_(const std::string &msg);

  // ── Temp conversion ───────────────────────────────────────────────────
  // Setpoint word for 0x54 D3/D4: °C × 10
  uint16_t setpoint_word_() const;
  // Room temperature word for 0x54 D5/D6 and 0x65 D1/D2: (°C + 50) × 10
  uint16_t room_word_() const;
  // The heater's own room temperature, °C; NAN without a usable reading
  float own_room_temperature_() const;
  void publish_room_temperature_();

  // ── Sensors ───────────────────────────────────────────────────────────
  text_sensor::TextSensor    *heater_state_sensor_{nullptr};
  binary_sensor::BinarySensor *flame_sensor_{nullptr};
  binary_sensor::BinarySensor *flag_0x20_sensor_{nullptr};  // raw 0x2C4 D2 bit5, meaning unconfirmed
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  text_sensor::TextSensor    *fault_sensor_{nullptr};
  sensor::Sensor             *current_temp_sensor_{nullptr};
  // Diagnostic (UDS) entities
  binary_sensor::BinarySensor *flame_detected_sensor_{nullptr};
  text_sensor::TextSensor     *combustion_state_sensor_{nullptr};
  sensor::Sensor              *flame_temperature_sensor_{nullptr};
  sensor::Sensor              *intake_temperature_sensor_{nullptr};
  sensor::Sensor              *supply_voltage_sensor_{nullptr};
  sensor::Sensor              *fuel_pump_frequency_sensor_{nullptr};
  binary_sensor::BinarySensor *glow_plug_sensor_{nullptr};
  sensor::Sensor              *glow_plug_power_sensor_{nullptr};
  text_sensor::TextSensor     *combustion_phase_sensor_{nullptr};
  sensor::Sensor              *phase_step_sensor_{nullptr};
  sensor::Sensor              *phase_time_sensor_{nullptr};
  sensor::Sensor              *blower_level_sensor_{nullptr};
  sensor::Sensor              *fuel_rate_sensor_{nullptr};
  sensor::Sensor              *fuel_total_sensor_{nullptr};
  sensor::Sensor              *power_demand_sensor_{nullptr};
  sensor::Sensor              *burn_time_sensor_{nullptr};
  sensor::Sensor              *heater_setpoint_sensor_{nullptr};
  sensor::Sensor              *heater_temperature_sensor_{nullptr};
  sensor::Sensor              *control_unit_temperature_sensor_{nullptr};
  sensor::Sensor              *air_pressure_sensor_{nullptr};
  sensor::Sensor              *operating_hours_sensor_{nullptr};
  // FD17 words 2–7: 0–25 %, 25–50 %, 50–75 %, 75–100 % power, control pause, altitude mode
  sensor::Sensor              *band_hours_sensor_[6]{};
  sensor::Sensor              *start_stop_count_sensor_{nullptr};
  sensor::Sensor              *fuel_pump_strokes_sensor_{nullptr};
  text_sensor::TextSensor     *software_version_sensor_{nullptr};
  text_sensor::TextSensor     *hardware_version_sensor_{nullptr};
  text_sensor::TextSensor     *part_number_sensor_{nullptr};
  text_sensor::TextSensor     *fault_memory_sensor_{nullptr};

  // ── Config ────────────────────────────────────────────────────────────
  // When true, the cabin temperature sensor value is in °F and will be
  // converted to °C internally.
  bool  cabin_temp_fahrenheit_{false};
  // When true, all outbound CAN frames are suppressed (TX standby).
  // Use when a real EasyStart Pro is on the bus: the WeAct emulates the Pro's
  // CAN IDs; both transmitting has been seen to trigger P000342.
  bool  tx_standby_{false};
  // true = report current_temperature_sensor as the control unit's room sensor;
  // false = hand back the heater's own sensor.
  bool  room_sensor_controller_{false};
  bool  room_from_controller_() const { return room_sensor_controller_ && current_temp_sensor_ != nullptr; }
  float    ext_temp_c_{NAN};          // last current_temperature_sensor value, °C
  float    intake_temp_c_{NAN};       // FD01 word 2
  uint32_t t_intake_{0};              // when it was read
  uint16_t own_room_word_{0xFFFE};    // 0x2C4 D5/D6 while the heater idles and nothing is commanded
  uint8_t  diag_phase_{0xFF};         // last FD10 byte 1 (0xFF = unknown)
  uint32_t t_diag_phase_{0};
  bool     settings_dirty_{false};    // room sensor selection changed: resend the settings frames
  uint8_t  settings_change_ctr_{0};   // 0x63 D4, counts 1→2→3→1 with every change (0 after power-up)
  // Controller identity. Defaults = the maintainer's EasyStart Pro. The heater
  // accepted any ID tested (incl. made-up and MAC-derived) and doesn't check 0x62 D8.
  uint8_t ctrl_id_[3]{0x82, 0xB6, 0x09};  // 0x60D D3–D5
  uint8_t init_62_last_{0xF1};            // 0x62 D8

  // ── Other controller on the bus (e.g. a real EasyStart Pro) ───────────
  bool     foreign_ctrl_present_{false};
  bool     foreign_fault_published_{false};
  uint32_t foreign_ctrl_last_ms_{0};
  uint8_t  foreign_ctrl_id_[3]{};
  void     note_foreign_controller_(const twai_message_t &msg);

  // ── Cooldown (D1=IDLE, D2 bit3 set, D3=0) ─────────────────────────────
  bool     cooldown_{false};
  void restart_handshake_();

  // ── CAN state ────────────────────────────────────────────────────────
  uint8_t  heater_d1_state_{0};
  uint8_t  heater_fault_code_{0};      // 0x2C4 D3: 0x00=ok, 0x20=flame loss, 0x40=overtemp
  bool     flame_active_{false};
  bool     flag_0x20_{false};
  bool     heater_connected_{false};

  // ── IDLE lock (D1=IDLE with D3≠0, not following a 0x0B fault) ─────────
  // Seen after every heater power interruption (D3=0x04): the heater ignores
  // HEAT/FAN until 0x54 goes from a non-idle command back to IDLE. We detect
  // it and send a short FAN→IDLE pulse to release it.
  bool     auto_clear_idle_lock_{true};
  bool     post_fault_window_{false};  // set on D1=0x0B; D3 then lingers in IDLE
  bool     idle_lock_{false};
  uint8_t  idle_lock_phase_{0};        // 0=wait, 1=pulse FAN, 2=force IDLE
  uint8_t  idle_lock_attempts_{0};
  uint32_t idle_lock_t_{0};
  uint32_t idle_lock_next_try_{0};
  bool     idle_lock_gave_up_{false};
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
  // Bus-error log rate limit (one summary per BUS_ERR_LOG_MS)
  uint32_t bus_err_log_ms_{0};
  uint32_t bus_err_logged_count_{0};
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

  // ── UDS diagnostics ───────────────────────────────────────────────────
  bool     diag_enabled_{false};      // `diagnostics:` block present
  uint32_t diag_interval_ms_{10000};  // sweep period while the heater is active
  bool     diag_sweep_active_{false};
  bool     diag_sweep_requested_{true};       // sweep once right after connecting / going idle
  bool     diag_fault_read_requested_{true};  // fault memory is read on demand, not every sweep
  uint8_t  diag_sweep_pos_{0};
  uint32_t t_diag_sweep_last_{0};
  uint32_t t_diag_hours_last_{0};
  bool     diag_hours_read_{false};
  uint32_t diag_unsupported_{0};      // bit per DiagItem the heater rejected
  uint32_t diag_read_once_{0};        // bit per DiagItem that never changes and has been read
  uint8_t  diag_timeouts_{0};         // consecutive unanswered requests
  bool     diag_waiting_{false};      // request in flight
  uint8_t  diag_pending_{0};          // its DiagItem
  uint32_t t_diag_request_{0};
  uint32_t t_diag_done_{0};
  // ISO-TP reassembly
  uint8_t  diag_buf_[DIAG_BUF_SIZE];
  uint16_t diag_len_{0};
  uint16_t diag_expected_{0};
  uint8_t  diag_next_seq_{0};
};

}  // namespace espar_can
}  // namespace esphome
