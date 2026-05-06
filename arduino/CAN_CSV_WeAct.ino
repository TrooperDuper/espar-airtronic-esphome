/* =============================================================================
 *  Espar Heater CAN Controller — WeAct CAN485 (ESP32 TWAI)
 *  Based on RE captures through Phase 2 Step 2J
 * =============================================================================
 *
 *  Wiring (WeAct CAN485 V1.0):
 *    RX_PIN = GPIO26 (CAN_RXD)
 *    TX_PIN = GPIO27 (CAN_TXD)
 *
 *  Serial monitor: 115200 baud
 *
 * ---------------------------------------------------------------------------
 *  SERIAL COMMANDS (send via Serial monitor or ESPHome UART service):
 *
 *    HEAT:XX    — Heat to XX degrees Fahrenheit  (e.g. "HEAT:72")
 *                 Valid range: 50 – 95 °F
 *    FAN        — Run blower only (no heat)
 *    OFF        — Stop (send idle command; after FAN mode expect ~75s cooldown)
 *    STATUS     — Print current heater state
 *    LOG:ON     — Enable raw CAN frame logging (SavvyCAN CSV format)
 *    LOG:OFF    — Disable raw CAN frame logging
 *
 * ---------------------------------------------------------------------------
 *  CAN TRAFFIC MAP SUMMARY (derived from RE captures)
 *
 *  HEATER → CONTROLLER (receive only):
 *    0x625  ~100ms  Heater alive heartbeat
 *                   D1=0x25 always | D2 toggles 0x00/0x10 | D3 session counter
 *    0x2C4  ~220ms  Heater primary status
 *                   D1: 0x02=startup, 0x03=idle, 0x09=heating
 *                   D2 bit5: flame active (D2 & 0x20)
 *                   D5/D6: 16-bit LE counter — ECHO BACK in 0x54 D5/D6
 *    0x2C5  ~220ms  Heater secondary status (all zeros observed)
 *    0x2C6  ~220ms  Heater data  (D1=0x32, D2=0x00, D3=0x7C, D4=0x01 typical)
 *
 *  CONTROLLER → HEATER (transmit):
 *    0x54   200ms   Primary command  (D2 selects mode)
 *                   IDLE:    [00][00][FE][FF][ctr_lo][ctr_hi][00][00]   D2=0x00
 *                   FAN:     [01][02][FE][FF][ctr_lo][ctr_hi][00][00]   D2=0x02
 *                   HEATING: [01][05][temp_lo][temp_hi][ctr_lo][ctr_hi][00][00] D2=0x05
 *                   temp = round((°F - 32) * 50 / 9)  as uint16 LE
 *    0x55   200ms   Config A — static: 10 27 00 00 00 00 00 00
 *    0x56   200ms   Config B — static: 10 27 00 00 00 00 00 00
 *    0x57   200ms   Config C — static: 00 00 FE FF FE FF 00 00
 *    0x60D  100ms   Controller heartbeat — static: 0D 10 82 B6 09 20 70 00
 *    0x65   ~10s    Periodic status burst (4 msgs per group, D3 cycles 01→02→03)
 *                   [ctr_lo][ctr_hi][D3][00][00][00][00][00]
 *
 *  INIT BURST (sent at startup, repeated every INIT_RESEND_MS until heater acks):
 *    0x5C 0x5F: 1E 00 1E 00 00 00 00 00
 *    0x5D 0x60: 00 00 00 00 00 00 FE FF
 *    0x5E 0x61: 00 00 00 00 00 00 00 00
 *    0x62:      12 00 00 00 00 00 00 F1
 *    0x63:      32 01 01 00 00 00 00 00
 *    0x64:      3C 00 05 00 C8 00 FF 03
 *    0x66:      00 00 00 00 00 00 00 00
 *    0x67-0x6C: BE 3E 27 00 01 1E 00 00
 *    0x6D:      12 00 00 00 00 00 00 F0
 *    0x10A:     10 00 01 20 10 01 00 00
 *
 *  TEMPERATURE PRESETS (confirmed from captures):
 *    66°F → 0x00BD  (D3=BD D4=00)
 *    75°F → 0x00EF  (D3=EF D4=00)
 *    78°F → 0x0100  (D3=00 D4=01)
 *    80°F → 0x010B  (D3=0B D4=01)
 * =============================================================================
 */

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "driver/twai.h"

// ── Pin definitions ──────────────────────────────────────────────────────────
#define RX_PIN  26
#define TX_PIN  27

// ── Timing constants (ms) ────────────────────────────────────────────────────
#define INTERVAL_60D_MS       100   // Controller heartbeat
#define INTERVAL_CMD_MS       200   // 0x54 / 0x55 / 0x56 / 0x57
#define INTERVAL_STATUS_MS  10000   // 0x65 burst (every 10s)
#define INIT_RESEND_MS        150   // Resend init burst until heater acks
#define INIT_STOP_AFTER_MS   5000   // Stop resending init after 5s of heater contact
#define STATUS_BURST_COUNT      4   // Frames per 0x65 burst group
#define STATUS_BURST_INTV_MS  150   // Spacing between frames in burst

// ── Heater state constants ───────────────────────────────────────────────────
#define HEATER_STATE_STARTUP  0x02
#define HEATER_STATE_IDLE     0x03
#define HEATER_STATE_HEATING  0x09
#define HEATER_STATE_FAN      0x21

// ── Global state ─────────────────────────────────────────────────────────────
static bool     driver_installed  = false;
static uint32_t start_ms          = 0;

// Command state
static bool     cmd_heating       = false;    // true = heating mode
static bool     cmd_fan           = false;    // true = fan-only mode (cmd_heating overrides)
static uint16_t cmd_temp_c_x10    = 0;        // target temp ×10 in °C
static float    cmd_temp_f        = 0;        // target temp in °F (for display)

// Counter echoed from heater's 0x2C4 D5/D6
static uint16_t heater_ctr        = 0xFFFE;  // default until heater connects
static bool     heater_connected  = false;
static uint32_t heater_last_seen  = 0;

// Heater status
static uint8_t  heater_state      = 0;
static bool     flame_active      = false;

// Timers
static uint32_t t_60D_last        = 0;
static uint32_t t_cmd_last        = 0;
static uint32_t t_status_last     = 0;
static uint32_t t_init_last       = 0;
static bool     init_done         = false;    // true after init burst sent at least once

// 0x65 sequencing
static uint8_t  status_seq        = 0;        // 0=D3:01, 1=D3:02, 2=D3:03
static uint8_t  status_burst_idx  = 0;        // how many frames sent this burst
static uint32_t t_burst_frame_last = 0;
static bool     burst_in_progress = false;

// CSV log toggle
static bool     csv_log_enabled   = false;

// ── Forward declarations ──────────────────────────────────────────────────────
static void send_frame(uint32_t id, const uint8_t* data, uint8_t len, bool ext = false);
static void send_init_burst();
static void send_heartbeat_60D();
static void send_command_group();
static void handle_status_burst(uint32_t now);
static void handle_rx();
static void handle_serial();
static void parse_0x2C4(const twai_message_t& msg);
static void print_csv_frame(const twai_message_t& msg);
static uint16_t fahrenheit_to_can(float f);
static void print_status();

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println(F("# Espar CAN Controller — WeAct CAN485"));
  Serial.println(F("# Commands: HEAT:XX  FAN  OFF  STATUS  LOG:ON  LOG:OFF"));

  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
    Serial.println(F("# ERROR: TWAI driver install failed"));
    return;
  }
  if (twai_start() != ESP_OK) {
    Serial.println(F("# ERROR: TWAI start failed"));
    return;
  }

  uint32_t alerts = TWAI_ALERT_RX_DATA | TWAI_ALERT_ERR_PASS |
                    TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL;
  if (twai_reconfigure_alerts(alerts, NULL) != ESP_OK) {
    Serial.println(F("# ERROR: Alert config failed"));
    return;
  }

  driver_installed = true;
  start_ms         = millis();

  // Send init burst immediately at startup
  send_init_burst();
  init_done   = true;
  t_init_last = millis();

  Serial.println(F("# Init burst sent. Waiting for heater..."));
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  if (!driver_installed) { delay(1000); return; }

  uint32_t now = millis();

  // ── Process incoming CAN frames ──
  handle_rx();

  // ── Re-send init burst until heater acks (or timeout) ──
  if (!heater_connected) {
    if ((now - t_init_last) >= INIT_RESEND_MS) {
      send_init_burst();
      t_init_last = now;
    }
  }

  // ── Heartbeat 0x60D every 100ms ──
  if ((now - t_60D_last) >= INTERVAL_60D_MS) {
    send_heartbeat_60D();
    t_60D_last = now;
  }

  // ── Command group (0x54/55/56/57) every 200ms ──
  if ((now - t_cmd_last) >= INTERVAL_CMD_MS) {
    send_command_group();
    t_cmd_last = now;
  }

  // ── 0x65 status burst every 10s ──
  handle_status_burst(now);

  // ── Serial input ──
  handle_serial();
}

// ─────────────────────────────────────────────────────────────────────────────
// TRANSMIT helpers
// ─────────────────────────────────────────────────────────────────────────────

static void send_frame(uint32_t id, const uint8_t* data, uint8_t len, bool ext) {
  twai_message_t msg = {};
  msg.identifier        = id;
  msg.extd              = ext ? 1 : 0;
  msg.data_length_code  = len;
  for (int i = 0; i < len && i < 8; i++) msg.data[i] = data[i];
  twai_transmit(&msg, pdMS_TO_TICKS(5));
}

// ── Init burst (0x5C-0x10A) ───────────────────────────────────────────────────
static void send_init_burst() {
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

  send_frame(0x5C, d5C, 8);
  send_frame(0x5F, d5C, 8);   // 0x5F same payload as 0x5C
  send_frame(0x62, d62, 8);
  send_frame(0x63, d63, 8);
  send_frame(0x66, d66, 8);
  send_frame(0x6D, d6D, 8);
  send_frame(0x10A,d10A,6);
  send_frame(0x5D, d5D, 8);
  send_frame(0x5E, d5E, 8);
  send_frame(0x60, d5D, 8);   // 0x60 same as 0x5D
  send_frame(0x61, d5E, 8);   // 0x61 same as 0x5E
  send_frame(0x64, d64, 8);
  send_frame(0x67, d67, 8);
  send_frame(0x68, d67, 8);
  send_frame(0x69, d67, 8);
  send_frame(0x6A, d67, 8);
  send_frame(0x6B, d67, 8);
  send_frame(0x6C, d67, 8);
}

// ── Controller heartbeat 0x60D ────────────────────────────────────────────────
static void send_heartbeat_60D() {
  static const uint8_t d[] = {0x0D,0x10,0x82,0xB6,0x09,0x20,0x70,0x00};
  send_frame(0x60D, d, 8);
}

// ── Primary command group (0x54, 0x55, 0x56, 0x57) ────────────────────────────
static void send_command_group() {
  uint8_t ctr_lo = (uint8_t)(heater_ctr & 0xFF);
  uint8_t ctr_hi = (uint8_t)((heater_ctr >> 8) & 0xFF);

  // 0x54 — primary command
  uint8_t d54[8] = {};
  if (cmd_heating) {
    d54[0] = 0x01;
    d54[1] = 0x05;
    d54[2] = (uint8_t)(cmd_temp_c_x10 & 0xFF);
    d54[3] = (uint8_t)((cmd_temp_c_x10 >> 8) & 0xFF);
  } else if (cmd_fan) {
    d54[0] = 0x01;
    d54[1] = 0x02;
    d54[2] = 0xFE;
    d54[3] = 0xFF;
  } else {
    d54[0] = 0x00;
    d54[1] = 0x00;
    d54[2] = 0xFE;
    d54[3] = 0xFF;
  }
  d54[4] = ctr_lo;
  d54[5] = ctr_hi;
  d54[6] = 0x00;
  d54[7] = 0x00;
  send_frame(0x54, d54, 8);

  // 0x55 — Config A (static)
  static const uint8_t d55[] = {0x10,0x27,0x00,0x00,0x00,0x00,0x00,0x00};
  send_frame(0x55, d55, 8);

  // 0x56 — Config B (static)
  static const uint8_t d56[] = {0x10,0x27,0x00,0x00,0x00,0x00,0x00,0x00};
  send_frame(0x56, d56, 8);

  // 0x57 — Config C (static)
  static const uint8_t d57[] = {0x00,0x00,0xFE,0xFF,0xFE,0xFF,0x00,0x00};
  send_frame(0x57, d57, 8);
}

// ── 0x65 periodic status burst (4 msgs, D3 cycles 01→02→03) ──────────────────
static void handle_status_burst(uint32_t now) {
  // Time to start a new burst?
  if (!burst_in_progress) {
    if ((now - t_status_last) >= INTERVAL_STATUS_MS) {
      burst_in_progress  = true;
      status_burst_idx   = 0;
      t_burst_frame_last = now;
    }
    return;
  }

  // Send next frame in burst
  if ((now - t_burst_frame_last) >= STATUS_BURST_INTV_MS || status_burst_idx == 0) {
    uint8_t d3_vals[] = {0x01, 0x02, 0x03};
    uint8_t ctr_lo = (uint8_t)(heater_ctr & 0xFF);
    uint8_t ctr_hi = (uint8_t)((heater_ctr >> 8) & 0xFF);
    uint8_t d65[8] = {ctr_lo, ctr_hi, d3_vals[status_seq], 0x00, 0x00, 0x00, 0x00, 0x00};
    send_frame(0x65, d65, 8);
    t_burst_frame_last = now;
    status_burst_idx++;

    if (status_burst_idx >= STATUS_BURST_COUNT) {
      // Burst complete — advance D3 sequence
      status_seq = (status_seq + 1) % 3;
      burst_in_progress = false;
      t_status_last     = now;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// RECEIVE handler
// ─────────────────────────────────────────────────────────────────────────────

static void handle_rx() {
  uint32_t alerts;
  twai_read_alerts(&alerts, 0);  // non-blocking poll

  twai_status_info_t st;
  twai_get_status_info(&st);

  if (alerts & TWAI_ALERT_ERR_PASS)
    Serial.printf("# Alert: error passive. Bus errors: %lu\n", st.bus_error_count);
  if (alerts & TWAI_ALERT_BUS_ERROR)
    Serial.printf("# Alert: bus error. Count: %lu\n", st.bus_error_count);
  if (alerts & TWAI_ALERT_RX_QUEUE_FULL)
    Serial.printf("# Alert: RX queue full. Missed: %lu\n", st.rx_missed_count);

  if (alerts & TWAI_ALERT_RX_DATA) {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
      if (csv_log_enabled) print_csv_frame(msg);
      if (msg.identifier == 0x2C4 && !msg.rtr && msg.data_length_code >= 6) {
        parse_0x2C4(msg);
      }
      if (msg.identifier == 0x625) {
        // Heater heartbeat — mark as connected
        if (!heater_connected) {
          heater_connected = true;
          Serial.println(F("# Heater heartbeat detected — connection established"));
        }
        heater_last_seen = millis();
      }
    }
  }

  // Detect heater loss
  if (heater_connected && (millis() - heater_last_seen) > 5000) {
    heater_connected = false;
    heater_ctr       = 0xFFFE;
    Serial.println(F("# WARNING: Heater heartbeat lost — resending init burst"));
    send_init_burst();
    t_init_last = millis();
  }
}

// ── Parse heater primary status (0x2C4) ──────────────────────────────────────
static void parse_0x2C4(const twai_message_t& msg) {
  uint8_t  new_state = msg.data[0];
  bool     new_flame = (msg.data[1] & 0x20) != 0;
  uint16_t new_ctr   = (uint16_t)msg.data[4] | ((uint16_t)msg.data[5] << 8);

  heater_ctr = new_ctr;  // echo counter back in our 0x54

  bool state_changed = (new_state != heater_state) || (new_flame != flame_active);
  heater_state  = new_state;
  flame_active  = new_flame;

  if (state_changed) {
    Serial.printf("# Heater status: %s | Flame: %s | Ctr: 0x%04X\n",
      (heater_state == HEATER_STATE_HEATING) ? "HEATING" :
      (heater_state == HEATER_STATE_FAN)     ? "FAN"     :
      (heater_state == HEATER_STATE_IDLE)    ? "IDLE"    :
      (heater_state == HEATER_STATE_STARTUP) ? "STARTUP" : "UNKNOWN",
      flame_active ? "ON" : "off",
      heater_ctr);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SERIAL command handler
// ─────────────────────────────────────────────────────────────────────────────

static void handle_serial() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toUpperCase();

  if (line.startsWith("HEAT:")) {
    float f = line.substring(5).toFloat();
    if (f < 50 || f > 95) {
      Serial.println(F("# ERROR: Temperature out of range (50-95°F)"));
      return;
    }
    cmd_temp_f       = f;
    cmd_temp_c_x10   = fahrenheit_to_can(f);
    cmd_heating      = true;
    cmd_fan          = false;
    Serial.printf("# Command: HEAT %.0f°F (CAN temp word = 0x%04X)\n",
                  cmd_temp_f, cmd_temp_c_x10);

  } else if (line == "FAN") {
    cmd_fan     = true;
    cmd_heating = false;
    Serial.println(F("# Command: FAN (blower only — no heat)"));

  } else if (line == "OFF") {
    cmd_heating = false;
    cmd_fan     = false;
    Serial.println(F("# Command: OFF"));

  } else if (line == "STATUS") {
    print_status();

  } else if (line == "LOG:ON") {
    csv_log_enabled = true;
    Serial.println(F("Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8"));

  } else if (line == "LOG:OFF") {
    csv_log_enabled = false;
    Serial.println(F("# CSV logging disabled"));

  } else if (line.length() > 0) {
    Serial.printf("# Unknown command: %s\n", line.c_str());
    Serial.println(F("# Commands: HEAT:XX  FAN  OFF  STATUS  LOG:ON  LOG:OFF"));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// UTILITY
// ─────────────────────────────────────────────────────────────────────────────

// Convert °F to Espar CAN temperature word (°C × 10, uint16 LE)
//   temp_c_x10 = round((F - 32) * 50 / 9)
//   Verified: 66°F→0x00BD  75°F→0x00EF  78°F→0x0100  80°F→0x010B
static uint16_t fahrenheit_to_can(float f) {
  float c_x10 = (f - 32.0f) * 50.0f / 9.0f;
  return (uint16_t)(c_x10 + 0.5f);  // round to nearest
}

static void print_status() {
  Serial.printf("# ── Espar Status ──────────────────────────────\n");
  Serial.printf("# Heater connected : %s\n", heater_connected ? "YES" : "NO");
  Serial.printf("# Heater state     : 0x%02X (%s)\n", heater_state,
    (heater_state == HEATER_STATE_HEATING) ? "HEATING" :
    (heater_state == HEATER_STATE_IDLE)    ? "IDLE"    :
    (heater_state == HEATER_STATE_STARTUP) ? "STARTUP" : "UNKNOWN");
  Serial.printf("# Flame active     : %s\n", flame_active ? "YES" : "NO");
  Serial.printf("# CAN counter      : 0x%04X\n", heater_ctr);
  Serial.printf("# Controller mode  : %s\n",
    cmd_heating ? "HEATING" : cmd_fan ? "FAN" : "IDLE");
  if (cmd_heating)
    Serial.printf("# Target temp      : %.0f°F (CAN 0x%04X)\n", cmd_temp_f, cmd_temp_c_x10);
  Serial.printf("# ─────────────────────────────────────────────\n");
}

// Print frame in SavvyCAN CSV format
static void print_csv_frame(const twai_message_t& msg) {
  uint32_t ts = (millis() - start_ms) * 1000UL;
  char d[8][3];
  for (int i = 0; i < 8; i++) {
    if (i < msg.data_length_code && !msg.rtr)
      snprintf(d[i], 3, "%02X", msg.data[i]);
    else
      d[i][0] = '\0';
  }
  Serial.printf("%lu,%08lX,%s,Rx,0,%d,%s,%s,%s,%s,%s,%s,%s,%s,\n",
    (unsigned long)ts,
    (unsigned long)msg.identifier,
    msg.extd ? "true" : "false",
    msg.data_length_code,
    d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
}
