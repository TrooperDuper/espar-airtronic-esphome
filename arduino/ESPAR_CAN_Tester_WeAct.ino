/*
 * ============================================================
 *  ESPAR GAS HEATER — CAN BUS TEST CONTROLLER
 *  Replaces OEM Espar controller for protocol validation
 * ============================================================
 *
 *  Hardware : WeAct CAN485 DevBoard V1  (ESP32-D0WD-V3, 8MB Flash)
 *             Chipanalog CA-IS2062A CAN transceiver (2.5kV isolated)
 *             Onboard 120Ω CAN termination (enable via onboard switch)
 *
 *  CAN Speed: 500 kbps  (confirmed Espar Airtronic S3 bus speed)
 *  Framework: Arduino IDE + ESP32 Board Package (>= 2.0.14)
 *
 *  BOARD SETTINGS (Arduino IDE):
 *    Board            → ESP32 Dev Module
 *    Upload Speed     → 921600
 *    CPU Frequency    → 240 MHz
 *    Flash Frequency  → 80 MHz
 *    Flash Mode       → QIO
 *    Flash Size       → 8MB (64Mb)  ← important for WeAct 8MB chip
 *    Partition Scheme → 8M with spiffs (3MB APP/1.5MB SPIFFS)
 *    Port             → your COM port / /dev/ttyACM0
 *
 *  NO EXTERNAL LIBRARIES REQUIRED.
 *  The TWAI (CAN) driver is built into the ESP32 Arduino core >= 2.0.x.
 *
 *  RGB LED:
 *    "Adafruit NeoPixel" library for status LED support.
 *    
 *  PURPOSE:
 *    This sketch completely replaces the OEM Espar controller.
 *    It sends all required heartbeat, init, and command messages,
 *    and provides a Serial Monitor interface to command the heater.
 *
 *  SAFETY:
 *    - Always starts in IDLE (OFF) mode
 *    - Temperature limits enforced: 55–85°F (configurable)
 *    - Watchdog: auto-OFF if heater goes silent for 10 seconds
 *    - 30-minute auto-shutoff (configurable via AUTO_OFF_MIN)
 *    - Emergency stop via 'E' or 'e' key at any time (no Enter needed)
 *    - All commands confirmed with heater status echo
 *
 *  QUICK START (Serial Monitor @ 115200 baud, NL+CR line endings):
 *    Type 'h'      → Start heating at default 66°F
 *    Type 'o'      → Stop heating
 *    Type 'h 78'   → Start heating at 78°F
 *    Type 's'      → Show current status
 *    Type '?'      → Full help menu
 *
 *  CONFIRMED TEMPERATURE ENCODINGS (Phase 2 validation):
 *    66°F → D3=0xBD D4=0x00  (Steps 2C–2E, multiple captures)
 *    75°F → D3=0xEF D4=0x00  (Step 2J, confirmed)
 *    78°F → D3=0x00 D4=0x01  (Steps 2F–2G, D4≠0 proves 16-bit LE)
 *    80°F → D3=0x0B D4=0x01  (Steps 2H–2I, confirmed)
 *
 *  DURATION NOTE:
 *    Duration (timer) is NOT transmitted on the CAN bus.
 *    The OEM controller stores it internally. The heater runs until
 *    commanded OFF. This sketch implements a configurable auto-off timer.
 *
 *  Protocol based on reverse engineering — see ESPAR_CAN_Analysis_Report.md
 *  Analysis date: 2026-04-04  |  Captures analyzed: Phase 2 Steps 2A–2J
 * ============================================================
 */

// ============================================================
//  DIAGNOSTIC: LISTEN-ONLY MODE
//  Uncomment to enable — ESP32 will ONLY receive, never transmit.
//  Use this if you see BUS OFF errors (b command shows BUS OFF).
//  In listen-only mode: no TX = no bus errors = no BUS OFF.
//  If you see [RX  ] frames in this mode, physical wiring is OK
//  and the problem was TX collision. Re-comment and re-flash to
//  restore normal control mode.
//
//  Leave commented out for normal operation.
// ============================================================
// #define DIAG_LISTEN_ONLY

// ============================================================
//  RGB LED (WS2812 on GPIO4)
//  Uncomment to enable — requires Adafruit NeoPixel library
// ============================================================
#define USE_NEOPIXEL

#ifdef USE_NEOPIXEL
#include <Adafruit_NeoPixel.h>
#define LED_PIN        4      // WS2812 on WeAct CAN485 board
#define LED_BRIGHTNESS 20     // 0–255 (keep low — very bright at full)
Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

#include "driver/twai.h"
#include <Arduino.h>

// ============================================================
//  PIN CONFIGURATION — WeAct CAN485 DevBoard V1
// ============================================================
//
//  The WeAct CAN485 DevBoard V1 is a self-contained board:
//    - ESP32-D0WD-V3 chip onboard (no separate ESP32 needed)
//    - CA-IS2062A CAN transceiver onboard (2.5kV isolated)
//    - Onboard 120Ω CAN termination resistor (enable with the switch)
//    - GPIO4 = WS2812 RGB LED (NOT a CAN pin!)
//    - GPIO26 = CAN RX (from transceiver to ESP32)
//    - GPIO27 = CAN TX (from ESP32 to transceiver)
//
//  Wiring to Espar harness:
//    CANH terminal → Espar CANH wire
//    CANL terminal → Espar CANL wire
//    GND           → Heater chassis / supply GND
//    (Power the board via USB from your computer)
//
#define CAN_TX_PIN  GPIO_NUM_27   // ESP32 → CA-IS2062A CAN TX
#define CAN_RX_PIN  GPIO_NUM_26   // CA-IS2062A CAN RX → ESP32

// ============================================================
//  ESPAR CAN MESSAGE IDs  (all standard 11-bit frames)
// ============================================================
// Controller → Heater
#define ID_CMD          0x054   // Primary command (heat/off + setpoint)
#define ID_CFG_A        0x055   // Static config A  (10 27 00 00 00 00 00 00)
#define ID_CFG_B        0x056   // Static config B  (identical to 0x55)
#define ID_CFG_C        0x057   // Static config C  (00 00 FE FF FE FF 00 00)
#define ID_HEARTBEAT    0x60D   // Controller heartbeat (100ms)
#define ID_PERIODIC     0x065   // Periodic status burst (every 10s)
// Init burst IDs (sent once at startup)
#define ID_INIT_5C      0x05C
#define ID_INIT_5D      0x05D
#define ID_INIT_5E      0x05E
#define ID_INIT_5F      0x05F
#define ID_INIT_60      0x060
#define ID_INIT_61      0x061
#define ID_INIT_62      0x062
#define ID_INIT_63      0x063   // D3=0x01 = "heater present" flag (CRITICAL)
#define ID_INIT_64      0x064
#define ID_INIT_66      0x066
#define ID_INIT_67      0x067
#define ID_INIT_68      0x068
#define ID_INIT_69      0x069
#define ID_INIT_6A      0x06A
#define ID_INIT_6B      0x06B
#define ID_INIT_6C      0x06C
#define ID_INIT_6D      0x06D
#define ID_INIT_10A     0x10A
// Heater → Controller
#define ID_HTR_STATUS   0x2C4   // Primary heater status ← MOST IMPORTANT
#define ID_HTR_FAULT    0x2C5   // Fault status (always zeros = no fault)
#define ID_HTR_SENSOR   0x2C6   // Sensor/config data (static)
#define ID_HTR_BEAT     0x625   // Heater heartbeat (100ms)

// ============================================================
//  TIMING INTERVALS  (milliseconds)
// ============================================================
#define INTERVAL_CMD        200     // 0x54/55/56/57 send rate (~5 Hz)
#define INTERVAL_HEARTBEAT  100     // 0x60D send rate (10 Hz)
#define INTERVAL_PERIODIC   10000   // 0x65 burst period
#define WATCHDOG_TIMEOUT    10000   // Declare heater lost after this ms

// ============================================================
//  SAFETY LIMITS
// ============================================================
#define TEMP_MIN_F      55      // Absolute minimum setpoint (°F)
#define TEMP_MAX_F      85      // Absolute maximum setpoint (°F)
#define TEMP_DEFAULT_F  66      // Default setpoint on boot
#define AUTO_OFF_MIN    30      // Auto-off after N minutes (0 = disable)

// ============================================================
//  HEATER STATE CODES  (decoded from 0x2C4 D1 byte)
// ============================================================
#define HTR_STATE_STANDBY   0x02
#define HTR_STATE_IDLE      0x03
#define HTR_STATE_INIT      0x08   // Brief transition state (1-2 frames only)
#define HTR_STATE_HEATING   0x09
#define HTR_STATE_FAULT     0x0B   // FAULT — D3 holds fault code
#define HTR_STATE_FANONLY   0x21   // Fan-only running (no heat)

// D2 sub-state flags
#define HTR_FLAG_FLAME      0x20   // D2 bit 5 = flame confirmed (during HEATING)
#define HTR_FLAG_FAULT_ACT  0x08   // D2 = 0x08 during active FAULT and post-fault clearing

// D3 fault code bits (0x2C4 byte 2)
#define HTR_FAULT_NONE      0x00   // No fault
#define HTR_FAULT_FLAME     0x20   // Flame loss / fuel starvation (bit 5)
#define HTR_FAULT_OVERTEMP  0x40   // Overtemperature / exhaust blocked (bit 6)

// ============================================================
//  CONTROLLER MODE
// ============================================================
typedef enum {
  MODE_IDLE = 0,    // Sending OFF command on 0x54
  MODE_HEAT = 1     // Sending heating command on 0x54
} ControlMode;

// ============================================================
//  STARTUP TRAFFIC MONITOR
//  Auto-prints ALL CAN frames for the first N seconds so you
//  can see whether anything is reaching the board at all.
//  Set to 0 to disable. Turns off automatically once heater seen.
// ============================================================
#define STARTUP_MONITOR_SECS  60

// ============================================================
//  GLOBAL STATE
// ============================================================
ControlMode   g_mode           = MODE_IDLE;
int           g_setpoint_f     = TEMP_DEFAULT_F;
bool          g_verbose        = false;      // full verbose (all frames + TX)
bool          g_monitor        = true;       // startup monitor (RX all frames)
bool          g_can_ready      = false;
unsigned long g_total_rx       = 0;          // total frames received (any ID)

// Heater status (updated from 0x2C4)
uint8_t       g_htr_state      = 0;
uint8_t       g_htr_flags      = 0;
uint8_t       g_htr_fault_code = 0;    // D3: 0x00=ok, 0x20=flame loss, 0x40=overtemp
uint8_t       g_htr_ctr_lo     = 0;
uint8_t       g_htr_ctr_hi     = 0;
unsigned long g_last_htr_msg   = 0;
unsigned long g_last_beat_msg  = 0;
bool          g_htr_seen       = false;
bool          g_htr_fault_latch = false;  // Latches true on any fault; cleared by 'r'

// Timing trackers
unsigned long g_last_cmd_sent  = 0;
unsigned long g_last_hb_sent   = 0;
unsigned long g_last_per_sent  = 0;
unsigned long g_heat_start_ms  = 0;

// Heartbeat toggle (0x60D alternates bits)
uint8_t g_hb_toggle = 0;

// Counter for 0x65 periodic message
uint16_t g_periodic_ctr = 0x0269;

// Counter for 0x54 command (synced with heater echo)
uint16_t g_cmd_ctr = 0x026A;

// ============================================================
//  LED HELPERS
// ============================================================
#ifdef USE_NEOPIXEL
void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t scale = LED_BRIGHTNESS;
  led.setPixelColor(0, led.Color(
    (uint8_t)((r * scale) / 255),
    (uint8_t)((g * scale) / 255),
    (uint8_t)((b * scale) / 255)
  ));
  led.show();
}
void ledOff()              { ledSet(0,   0,   0);   }   // Off
void ledBlue()             { ledSet(0,   0,   255); }   // Idle / connected
void ledGreen()            { ledSet(0,   255, 0);   }   // Heating
void ledRed()              { ledSet(255, 0,   0);   }   // Error / watchdog
void ledYellow()           { ledSet(255, 200, 0);   }   // Setpoint changed
void ledPurple()           { ledSet(180, 0,   180); }   // Init
void ledUpdateStatus() {
  if (!g_can_ready)        { ledRed();    return; }
  if (g_mode == MODE_HEAT) { ledGreen();  return; }
  if (g_htr_seen)          { ledBlue();   return; }
  // Slow pulse white until heater seen
  uint8_t pulse = (uint8_t)((millis() / 8) & 0xFF);
  ledSet(pulse / 8, pulse / 8, pulse / 8);
}
#else
// Stubs — LED calls are no-ops without NeoPixel
void ledOff()              {}
void ledBlue()             {}
void ledGreen()            {}
void ledRed()              {}
void ledYellow()           {}
void ledPurple()           {}
void ledUpdateStatus()     {}
#endif

// ============================================================
//  HELPER: Build and send a standard CAN frame
// ============================================================
bool canSend(uint32_t id, const uint8_t* data, uint8_t len) {
  if (!g_can_ready) return false;

  twai_message_t msg;
  msg.identifier        = id;
  msg.extd              = 0;      // Standard 11-bit frame
  msg.rtr               = 0;      // Data frame (not remote)
  msg.data_length_code  = len;
  msg.ss                = 0;
  msg.self              = 0;
  msg.dlc_non_comp      = 0;
  memset(msg.data, 0, 8);
  memcpy(msg.data, data, len);

  esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(5));
  if (err != ESP_OK && g_verbose) {
    Serial.printf("[WARN] TX failed ID=0x%03X err=0x%02X\n", (unsigned)id, (unsigned)err);
  }
  return (err == ESP_OK);
}

// ============================================================
//  TEMPERATURE ENCODE/DECODE
//
//  Encoding confirmed at 4 setpoints (Phase 2 Steps 2C–2J):
//    Format: little-endian uint16, units = 0.1°C per LSB
//    D3 = low byte,  D4 = high byte
//
//    66°F → 18.9°C × 10 = 189 = 0x00BD → D3=0xBD D4=0x00
//    75°F → 23.9°C × 10 = 239 = 0x00EF → D3=0xEF D4=0x00
//    78°F → 25.6°C × 10 = 256 = 0x0100 → D3=0x00 D4=0x01  ← D4 non-zero!
//    80°F → 26.7°C × 10 = 267 = 0x010B → D3=0x0B D4=0x01
// ============================================================
void encodeTempF(int tempF, uint8_t* lo, uint8_t* hi) {
  float  tempC = (tempF - 32.0f) * 5.0f / 9.0f;
  int    raw   = (int)(tempC * 10.0f + 0.5f);
  *lo = (uint8_t)(raw & 0xFF);
  *hi = (uint8_t)((raw >> 8) & 0xFF);
}

float decodeTempRaw(uint8_t lo, uint8_t hi) {
  int   raw   = (int)((hi << 8) | lo);
  float tempC = raw / 10.0f;
  return tempC * 9.0f / 5.0f + 32.0f;
}

// ============================================================
//  SEND: 0x54 — Primary Command Message
//  Sent at ~5 Hz continuously. This is THE control message.
// ============================================================
void sendCmd54() {
  uint8_t data[8];

  if (g_mode == MODE_HEAT) {
    uint8_t tlo, thi;
    encodeTempF(g_setpoint_f, &tlo, &thi);
    data[0] = 0x01;                           // D1: heating command ON
    data[1] = 0x05;                           // D2: heating mode
    data[2] = tlo;                            // D3: temp setpoint low byte
    data[3] = thi;                            // D4: temp setpoint high byte
    data[4] = (uint8_t)(g_cmd_ctr & 0xFF);   // D5: counter low (heater echoes this)
    data[5] = (uint8_t)(g_cmd_ctr >> 8);     // D6: counter high
    data[6] = 0x00;
    data[7] = 0x00;
  } else {
    // IDLE / OFF command — SAE J1939 "Not Available" sentinel values
    data[0] = 0x00;
    data[1] = 0x00;
    data[2] = 0xFE;
    data[3] = 0xFF;
    data[4] = 0xFE;
    data[5] = 0xFF;
    data[6] = 0x00;
    data[7] = 0x00;
  }

  canSend(ID_CMD, data, 8);
}

// ============================================================
//  SEND: 0x55, 0x56, 0x57 — Static Config Companions
//  Must accompany 0x54. Payloads never change.
// ============================================================
void sendCfg55_57() {
  const uint8_t d55[8] = {0x10, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t d56[8] = {0x10, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t d57[8] = {0x00, 0x00, 0xFE, 0xFF, 0xFE, 0xFF, 0x00, 0x00};
  canSend(ID_CFG_A, d55, 8);
  canSend(ID_CFG_B, d56, 8);
  canSend(ID_CFG_C, d57, 8);
}

// ============================================================
//  SEND: 0x60D — Controller Heartbeat  (100ms)
//  Toggles 2 bits across 4-state pattern to show liveness.
// ============================================================
void sendHeartbeat60D() {
  const uint8_t hb_states[4][8] = {
    {0x0D, 0x10, 0x83, 0xB6, 0x09, 0x20, 0x70, 0x00},
    {0x0D, 0x11, 0x82, 0xB6, 0x09, 0x20, 0x70, 0x00},
    {0x0D, 0x10, 0x82, 0xB6, 0x09, 0x20, 0x70, 0x00},
    {0x0D, 0x11, 0x83, 0xB6, 0x09, 0x20, 0x70, 0x00},
  };
  canSend(ID_HEARTBEAT, hb_states[g_hb_toggle & 0x03], 8);
  g_hb_toggle++;
}

// ============================================================
//  SEND: 0x65 — Periodic Status Burst  (every 10 seconds)
//  Sends 4 copies of each sub-type (D3 = 01, 02, 03).
// ============================================================
void sendPeriodic65() {
  uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (uint8_t subtype = 1; subtype <= 3; subtype++) {
    data[0] = (uint8_t)(g_periodic_ctr & 0xFF);
    data[1] = (uint8_t)(g_periodic_ctr >> 8);
    data[2] = subtype;
    for (int i = 0; i < 4; i++) {
      canSend(ID_PERIODIC, data, 8);
      delay(2);
    }
    g_periodic_ctr++;
  }
}

// ============================================================
//  SEND: Initialization Burst  (one-time at startup)
//  Mimics OEM controller power-up sequence.
//  0x63 D3=0x01 = "heater present" flag — do not change.
// ============================================================
void sendInitBurst() {
  Serial.println(F("[INIT] Sending initialization burst..."));
  ledPurple();

  const uint8_t d5C[8]  = {0x1E, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t d5D[8]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF};
  const uint8_t d5E[8]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t d5F[8]  = {0x1E, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t d60[8]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF};
  const uint8_t d61[8]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t d62[8]  = {0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF1};
  const uint8_t d63[8]  = {0x32, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}; // D3=0x01 CRITICAL
  const uint8_t d64[8]  = {0x3C, 0x00, 0x05, 0x00, 0xC8, 0x00, 0xFF, 0x03};
  const uint8_t d66[8]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t d67[8]  = {0xBE, 0x3E, 0x27, 0x00, 0x01, 0x1E, 0x00, 0x00};
  const uint8_t d6D[8]  = {0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0};
  const uint8_t d10A[8] = {0x10, 0x00, 0x01, 0x20, 0x10, 0x01, 0x00, 0x00};

  canSend(ID_INIT_5C,  d5C,  8); delay(5);
  canSend(ID_INIT_5D,  d5D,  8); delay(5);
  canSend(ID_INIT_5E,  d5E,  8); delay(5);
  canSend(ID_INIT_5F,  d5F,  8); delay(5);
  canSend(ID_INIT_60,  d60,  8); delay(5);
  canSend(ID_INIT_61,  d61,  8); delay(5);
  canSend(ID_INIT_62,  d62,  8); delay(5);
  canSend(ID_INIT_63,  d63,  8); delay(5);
  canSend(ID_INIT_64,  d64,  8); delay(5);
  canSend(ID_INIT_66,  d66,  8); delay(5);
  canSend(ID_INIT_67,  d67,  8); delay(5);
  canSend(ID_INIT_68,  d67,  8); delay(5);  // 0x68–0x6C all identical to 0x67
  canSend(ID_INIT_69,  d67,  8); delay(5);
  canSend(ID_INIT_6A,  d67,  8); delay(5);
  canSend(ID_INIT_6B,  d67,  8); delay(5);
  canSend(ID_INIT_6C,  d67,  8); delay(5);
  canSend(ID_INIT_6D,  d6D,  8); delay(5);
  canSend(ID_INIT_10A, d10A, 8); delay(5);

  Serial.println(F("[INIT] Init burst complete."));
}

// ============================================================
//  CONTROL: Set controller mode (IDLE or HEAT)
// ============================================================
void setMode(ControlMode mode) {
  if (mode == g_mode) return;

  if (mode == MODE_HEAT) {
    if (!g_htr_seen) {
      Serial.println(F("[WARN] Cannot start heat — no heater heartbeat received yet!"));
      Serial.println(F("       Check CANH/CANL wiring and heater power."));
      return;
    }
    if ((millis() - g_last_htr_msg) > WATCHDOG_TIMEOUT) {
      Serial.println(F("[WARN] Cannot start heat — heater status timed out."));
      return;
    }
    g_mode          = MODE_HEAT;
    g_heat_start_ms = millis();
    ledGreen();
    Serial.printf("[CMD ] >> HEAT ON @ %d°F (%.1f°C)\n",
                  g_setpoint_f,
                  (g_setpoint_f - 32.0f) * 5.0f / 9.0f);
  } else {
    g_mode = MODE_IDLE;
    ledBlue();
    Serial.println(F("[CMD ] >> HEAT OFF"));
  }
}

// ============================================================
//  CONTROL: Emergency Stop
// ============================================================
void emergencyStop() {
  g_mode = MODE_IDLE;
  const uint8_t off[8] = {0x00, 0x00, 0xFE, 0xFF, 0xFE, 0xFF, 0x00, 0x00};
  for (int i = 0; i < 5; i++) {
    canSend(ID_CMD, off, 8);
    delay(20);
  }
  ledRed();
  Serial.println(F(""));
  Serial.println(F("!!! EMERGENCY STOP !!!"));
  Serial.println(F("!!! Heater commanded OFF !!!"));
  Serial.println(F(""));
}

// ============================================================
//  CONTROL: Set temperature setpoint
// ============================================================
bool setSetpoint(int tempF) {
  if (tempF < TEMP_MIN_F || tempF > TEMP_MAX_F) {
    Serial.printf("[WARN] Temperature %d°F out of range (%d–%d°F)\n",
                  tempF, TEMP_MIN_F, TEMP_MAX_F);
    return false;
  }
  g_setpoint_f = tempF;
  uint8_t tlo, thi;
  encodeTempF(g_setpoint_f, &tlo, &thi);
  ledYellow();
  delay(100);
  ledUpdateStatus();
  Serial.printf("[CMD ] Setpoint: %d°F  (%.1f°C)  encoded: D3=0x%02X D4=0x%02X\n",
                g_setpoint_f,
                (g_setpoint_f - 32.0f) * 5.0f / 9.0f,
                tlo, thi);
  return true;
}

// ============================================================
//  DECODE & DISPLAY: 0x2C4 Heater Primary Status
// ============================================================
void process2C4(const twai_message_t& msg) {
  uint8_t  state      = msg.data[0];  // D1: heater operating state
  uint8_t  flags      = msg.data[1];  // D2: sub-state / flags
  uint8_t  fault_code = msg.data[2];  // D3: fault code (0x00=none during normal op)
  uint8_t  ctr_lo     = msg.data[4];  // D5: counter
  uint8_t  ctr_hi     = msg.data[5];  // D6: counter

  bool state_changed = (state != g_htr_state) || (flags != g_htr_flags)
                     || (fault_code != g_htr_fault_code);

  g_htr_state      = state;
  g_htr_flags      = flags;
  g_htr_fault_code = fault_code;
  g_htr_ctr_lo     = ctr_lo;
  g_htr_ctr_hi     = ctr_hi;
  g_last_htr_msg   = millis();
  g_htr_seen       = true;

  // ── Fault detection (active fault OR post-fault code still clearing) ──
  bool fault_active = (state == HTR_STATE_FAULT);
  bool fault_clearing = (state == HTR_STATE_IDLE && fault_code != HTR_FAULT_NONE);

  if (fault_active && !g_htr_fault_latch) {
    // First detection of fault state — command OFF immediately
    g_htr_fault_latch = true;
    ledRed();
    Serial.println(F("\n[FAULT] *** HEATER FAULT DETECTED ***"));
    Serial.printf(  "[FAULT] State=0x%02X  D2=0x%02X  FaultCode=0x%02X\n",
                    state, flags, fault_code);

    // Decode fault code
    if (fault_code & HTR_FAULT_FLAME) {
      Serial.println(F("[FAULT] Code: FLAME LOSS / FUEL STARVATION (D3 bit5=0x20)"));
      Serial.println(F("[FAULT] → Check: fuel line not kinked, fuel tank not empty"));
    }
    if (fault_code & HTR_FAULT_OVERTEMP) {
      Serial.println(F("[FAULT] Code: OVERTEMPERATURE / EXHAUST BLOCKED (D3 bit6=0x40)"));
      Serial.println(F("[FAULT] → Check: exhaust outlet is not blocked or restricted"));
    }
    if (fault_code == HTR_FAULT_NONE) {
      Serial.println(F("[FAULT] Code: 0x00 (unknown fault type — D3 not populated)"));
    }

    // Command heater to idle immediately
    if (g_mode == MODE_HEAT) {
      Serial.println(F("[FAULT] Commanding OFF for safety..."));
      g_mode = MODE_IDLE;
      g_heat_start_ms = 0;
    }
    Serial.println(F("[FAULT] Type 'r' to reset bus, 'f' to view fault status."));
  }

  if (state_changed) {
    if (!fault_active) {
      // Normal state change (not fault)
      Serial.print(F("\n[HTR ] Status changed → "));
      switch (state) {
        case HTR_STATE_STANDBY: Serial.print(F("STANDBY (startup)")); break;
        case HTR_STATE_INIT:    Serial.print(F("INIT (brief transition)")); break;
        case HTR_STATE_IDLE:
          if (fault_clearing) {
            Serial.print(F("IDLE (fault clearing, code still set)"));
          } else {
            Serial.print(F("IDLE (awaiting command)"));
            if (g_htr_fault_latch) {
              // Fault fully cleared
              Serial.print(F(" — fault code cleared ✓"));
              g_htr_fault_latch = false;
            }
          }
          break;
        case HTR_STATE_HEATING: Serial.print(F("HEATING ACTIVE *** ★ ***")); break;
        case HTR_STATE_FANONLY: Serial.print(F("FAN-ONLY RUNNING")); break;
        default:
          Serial.printf("UNKNOWN (0x%02X)", state);
          break;
      }

      if ((flags & HTR_FLAG_FLAME) && state == HTR_STATE_HEATING) {
        Serial.print(F("  [FLAME CONFIRMED 🔥]"));
      }

      // Counter sync validation
      uint16_t htr_ctr = ((uint16_t)ctr_hi << 8) | ctr_lo;
      if (g_mode == MODE_HEAT && htr_ctr == g_cmd_ctr) {
        Serial.print(F("  [COUNTER SYNC OK ✓]"));
      } else if (g_mode == MODE_HEAT) {
        Serial.print(F("  [DRIFT]"));
      }

      Serial.printf("\n         Raw: D1=0x%02X D2=0x%02X D3=0x%02X D5=0x%02X D6=0x%02X\n",
                    state, flags, fault_code, ctr_lo, ctr_hi);
    }
    ledUpdateStatus();
  }

  if (g_verbose) {
    Serial.printf("[2C4 ] state=0x%02X flags=0x%02X faultCode=0x%02X ctr=0x%02X%02X\n",
                  state, flags, fault_code, ctr_hi, ctr_lo);
  }
}

// ============================================================
//  DECODE & DISPLAY: 0x625 Heater Heartbeat
// ============================================================
void process625(const twai_message_t& msg) {
  g_last_beat_msg = millis();
  g_htr_seen      = true;
  if (g_verbose) {
    Serial.printf("[0x625] Heater heartbeat  D2=0x%02X D3=0x%02X\n",
                  msg.data[1], msg.data[2]);
  }
}

// ============================================================
//  DISPLAY: Raw frame printer (verbose mode)
// ============================================================
void printFrame(const twai_message_t& msg, const char* label) {
  Serial.printf("[%s] ID=0x%03X  ", label, (unsigned)msg.identifier);
  for (int i = 0; i < msg.data_length_code; i++) {
    Serial.printf("%02X ", msg.data[i]);
  }
  Serial.println();
}

// ============================================================
//  RECEIVE: Poll and process all incoming CAN frames
// ============================================================
void receiveMessages() {
  twai_message_t msg;

  // Auto-disable startup monitor once heater is seen, or after timeout
  if (g_monitor && g_htr_seen) {
    g_monitor = false;
    Serial.println(F("[MON ] Heater found — startup monitor off. Type 'v' for verbose."));
  }
  if (g_monitor && millis() > (unsigned long)STARTUP_MONITOR_SECS * 1000UL) {
    g_monitor = false;
    Serial.println(F("[MON ] Startup monitor window closed. Type 'v' to enable verbose."));
  }

  while (twai_receive(&msg, pdMS_TO_TICKS(0)) == ESP_OK) {
    g_total_rx++;

    // Startup monitor: print ALL received frames before heater is seen
    if (g_monitor) {
      Serial.printf("[RX  ] t=%lus  ID=0x%03X  ", millis()/1000, (unsigned)msg.identifier);
      for (int i = 0; i < msg.data_length_code; i++) {
        Serial.printf("%02X ", msg.data[i]);
      }
      Serial.println();
    }

    switch (msg.identifier) {
      case ID_HTR_STATUS:  process2C4(msg); break;
      case ID_HTR_BEAT:    process625(msg); break;
      case ID_HTR_FAULT:   /* all zeros, skip silently */  break;
      case ID_HTR_SENSOR:  /* static, verbose only */
        if (g_verbose) printFrame(msg, "2C6 ");
        break;
      default:
        if (g_verbose) printFrame(msg, "UNKN");
        break;
    }
  }
}

// ============================================================
//  WATCHDOG: Auto-OFF if heater stops responding
// ============================================================
void checkWatchdog() {
  if (!g_htr_seen) return;

  unsigned long now     = millis();
  unsigned long elapsed = now - g_last_htr_msg;

  if (elapsed > WATCHDOG_TIMEOUT && g_mode == MODE_HEAT) {
    ledRed();
    Serial.println(F("\n[WDOG] WARNING: Heater status timeout! Commanding OFF for safety."));
    emergencyStop();
    Serial.printf("[WDOG] Last heater message was %lu ms ago.\n", elapsed);
    Serial.println(F("[WDOG] Check CANH/CANL wiring and heater power."));
  }

  // Auto-off timer
  if (AUTO_OFF_MIN > 0 && g_mode == MODE_HEAT) {
    unsigned long auto_off_ms = (unsigned long)AUTO_OFF_MIN * 60000UL;
    if ((now - g_heat_start_ms) > auto_off_ms) {
      Serial.printf("\n[SAFE] %d-minute auto-shutoff triggered.\n", AUTO_OFF_MIN);
      setMode(MODE_IDLE);
    }
  }
}

// ============================================================
//  BUS STATUS: Print TWAI error counters
//  Use this when you can't see heater traffic — it tells you
//  if the CAN bus has electrical / termination problems.
// ============================================================
void printBusStatus() {
  twai_status_info_t status;
  esp_err_t err = twai_get_status_info(&status);

  Serial.println(F("\n╔══════════════════════════════════════════╗"));
  Serial.println(F("║         CAN BUS DIAGNOSTICS               ║"));
  Serial.println(F("╚══════════════════════════════════════════╝"));

  if (err != ESP_OK) {
    Serial.printf("  ERROR reading TWAI status: 0x%04X\n", (unsigned)err);
    return;
  }

  // State
  const char* state_str;
  switch (status.state) {
    case TWAI_STATE_STOPPED:    state_str = "STOPPED";             break;
    case TWAI_STATE_RUNNING:    state_str = "RUNNING (normal)";    break;
    case TWAI_STATE_BUS_OFF:    state_str = "BUS OFF  ← ERROR";   break;
    case TWAI_STATE_RECOVERING: state_str = "RECOVERING";          break;
    default:                    state_str = "UNKNOWN";             break;
  }
  Serial.printf("  Bus State       : %s\n", state_str);
  Serial.printf("  TX Error Count  : %lu  (>127 = warning, >255 = bus-off)\n",
                status.tx_error_counter);
  Serial.printf("  RX Error Count  : %lu\n", status.rx_error_counter);
  Serial.printf("  TX Failed       : %lu  (frames that couldn't send)\n",
                status.tx_failed_count);
  Serial.printf("  RX Missed       : %lu  (buffer overflows)\n",
                status.rx_missed_count);
  Serial.printf("  Arb Lost        : %lu  (lost bus arbitration)\n",
                status.arb_lost_count);
  Serial.printf("  Bus Errors      : %lu  (CRC/form/bit errors seen)\n",
                status.bus_error_count);
  Serial.printf("  Total RX frames : %lu\n", g_total_rx);

  // Diagnostic interpretation
  Serial.println();
  if (status.state == TWAI_STATE_BUS_OFF) {
    Serial.println(F("  !! BUS OFF: CAN bus shorted or no termination."));
    Serial.println(F("     Check CANH/CANL wiring and 120Ω termination switch."));
    Serial.println(F("     Type 'r' to attempt bus recovery."));
  } else if (status.tx_error_counter > 50 || status.bus_error_count > 10) {
    Serial.println(F("  !! High error counts — possible causes:"));
    Serial.println(F("     1. Wrong termination (enable 120R switch on board)"));
    Serial.println(F("     2. CANH/CANL swapped — try swapping the two wires"));
    Serial.println(F("     3. Bad GND connection to heater chassis"));
    Serial.println(F("     4. Bus speed mismatch (sketch uses 500kbps)"));
  } else if (g_total_rx == 0) {
    Serial.println(F("  !! Zero frames received — no CAN traffic seen at all."));
    Serial.println(F("     If bus errors are low, check:"));
    Serial.println(F("     1. Is the heater powered on?"));
    Serial.println(F("     2. Are CANH and CANL connected to the right wires?"));
    Serial.println(F("     3. Is GND shared with heater 12V supply ground?"));
  } else if (!g_htr_seen) {
    Serial.println(F("  Frames received but no heater IDs seen yet."));
    Serial.println(F("  Known heater IDs: 0x625, 0x2C4, 0x2C5, 0x2C6"));
    Serial.println(F("  Type 'v' to see all frame IDs."));
  } else {
    Serial.println(F("  Bus looks healthy."));
  }
  Serial.println();
}

// ============================================================
//  RECOVER: Attempt to recover from bus-off state
// ============================================================
void busRecover() {
  Serial.println(F("[BUS ] Attempting bus recovery..."));
  // Also clear fault latch so heating can be restarted after physical fix
  if (g_htr_fault_latch) {
    Serial.println(F("[BUS ] Clearing fault latch (heater fault acknowledged)."));
    g_htr_fault_latch = false;
  }
  twai_initiate_recovery();
  delay(200);
  twai_status_info_t s;
  twai_get_status_info(&s);
  Serial.printf("[BUS ] State after recovery attempt: %d\n", s.state);
  if (s.state == TWAI_STATE_RUNNING) {
    Serial.println(F("[BUS ] Recovery successful!"));
  } else {
    Serial.println(F("[BUS ] Still not running. Check wiring and restart board."));
  }
}

// ============================================================
//  STATUS: Print current state summary
// ============================================================
void printStatus() {
  Serial.println(F("\n╔══════════════════════════════════════════╗"));
  Serial.println(F("║      ESPAR CAN BUS STATUS (WeAct)        ║"));
  Serial.println(F("╚══════════════════════════════════════════╝"));

  Serial.printf("  Controller Mode : %s\n",
                g_mode == MODE_HEAT ? "HEATING ★" : "IDLE (OFF)");
  Serial.printf("  Setpoint        : %d°F  (%.1f°C)\n",
                g_setpoint_f,
                (g_setpoint_f - 32.0f) * 5.0f / 9.0f);

  if (!g_htr_seen) {
    Serial.println(F("  Heater Status   : NOT SEEN — check CANH/CANL wiring!"));
  } else {
    unsigned long age = millis() - g_last_htr_msg;
    const char* state_str;
    switch (g_htr_state) {
      case HTR_STATE_STANDBY: state_str = "STANDBY";          break;
      case HTR_STATE_INIT:    state_str = "INIT";             break;
      case HTR_STATE_IDLE:    state_str = "IDLE";             break;
      case HTR_STATE_HEATING: state_str = "HEATING \xe2\x98\x85"; break;
      case HTR_STATE_FAULT:   state_str = "*** FAULT ***";    break;
      case HTR_STATE_FANONLY: state_str = "FAN-ONLY";         break;
      default:                state_str = "UNKNOWN";          break;
    }
    Serial.printf("  Heater State    : %s (0x%02X)  %s\n",
                  state_str, g_htr_state,
                  (g_htr_flags & HTR_FLAG_FLAME) ? "[FLAME ACTIVE]" : "");

    // Fault status
    if (g_htr_fault_latch || g_htr_fault_code != HTR_FAULT_NONE) {
      Serial.printf("  !!! FAULT CODE  : 0x%02X —", g_htr_fault_code);
      if (g_htr_fault_code & HTR_FAULT_FLAME)    Serial.print(F(" FLAME LOSS"));
      if (g_htr_fault_code & HTR_FAULT_OVERTEMP) Serial.print(F(" OVERTEMP"));
      if (g_htr_fault_code == HTR_FAULT_NONE)    Serial.print(F(" (clearing)"));
      if (g_htr_state == HTR_STATE_IDLE)          Serial.print(F(" [post-fault clearing]"));
      Serial.println();
    }

    Serial.printf("  Last Seen       : %lu ms ago\n", age);
    Serial.printf("  Counter Echo    : 0x%02X%02X  (our cmd ctr: 0x%04X)  %s\n",
                  g_htr_ctr_hi, g_htr_ctr_lo, g_cmd_ctr,
                  (((uint16_t)g_htr_ctr_hi << 8 | g_htr_ctr_lo) == g_cmd_ctr)
                    ? "[SYNCED ✓]" : "[DRIFT]");
  }

  if (g_last_beat_msg > 0) {
    Serial.printf("  Heater Beat     : last seen %lu ms ago\n",
                  millis() - g_last_beat_msg);
  } else {
    Serial.println(F("  Heater Beat     : not received yet"));
  }

  unsigned long upSec = millis() / 1000;
  Serial.printf("  Uptime          : %lu:%02lu:%02lu\n",
                upSec / 3600, (upSec % 3600) / 60, upSec % 60);

  if (g_mode == MODE_HEAT && AUTO_OFF_MIN > 0) {
    unsigned long heatSec = (millis() - g_heat_start_ms) / 1000;
    unsigned long remSec  = (unsigned long)AUTO_OFF_MIN * 60 - heatSec;
    Serial.printf("  Heating for     : %lu:%02lu:%02lu  (auto-off in %lu:%02lu)\n",
                  heatSec / 3600, (heatSec % 3600) / 60, heatSec % 60,
                  remSec / 60, remSec % 60);
  }

  Serial.printf("  Total RX frames : %lu\n", g_total_rx);
  Serial.printf("  Verbose mode    : %s\n", g_verbose ? "ON" : "OFF");
  Serial.printf("  Startup monitor : %s\n", g_monitor ? "ON (auto)" : "OFF");

#ifdef USE_NEOPIXEL
  Serial.println(F("  RGB LED         : ENABLED (GPIO4)"));
#else
  Serial.println(F("  RGB LED         : disabled (uncomment #define USE_NEOPIXEL)"));
#endif

  // Quick bus health hint
  if (g_total_rx == 0) {
    Serial.println(F("\n  !! NO RX TRAFFIC — type 'b' for bus diagnostics."));
  }
  Serial.println();
}

// ============================================================
//  HELP MENU
// ============================================================
void printHelp() {
  Serial.println(F("\n╔════════════════════════════════════════════╗"));
  Serial.println(F("║    ESPAR CAN TESTER — WeAct CAN485 Board   ║"));
  Serial.println(F("╠════════════════════════════════════════════╣"));
  Serial.println(F("║  h          Start heating (current temp)   ║"));
  Serial.println(F("║  h <F>      Start heating at <F>°F         ║"));
  Serial.println(F("║  o          Stop heating (OFF)             ║"));
  Serial.println(F("║  t <F>      Set temperature only           ║"));
  Serial.println(F("║  s          Show current status            ║"));
  Serial.println(F("║  b          Bus error diagnostics ← debug  ║"));
  Serial.println(F("║  r          Attempt bus recovery           ║"));
  Serial.println(F("║  v          Toggle verbose (all frames)    ║"));
  Serial.println(F("║  m          Toggle startup monitor (RX)    ║"));
  Serial.println(F("║  i          Re-send init burst             ║"));
  Serial.println(F("║  f          Fault status & code details    ║"));
  Serial.println(F("║  e  or  E   EMERGENCY STOP (instant)       ║"));
  Serial.println(F("║  ?          Show this help                 ║"));
  Serial.println(F("╠════════════════════════════════════════════╣"));
  Serial.println(F("║  TROUBLESHOOTING — no heater seen?         ║"));
  Serial.println(F("║  1. Type 'b' — check bus error counters    ║"));
  Serial.println(F("║  2. Type 'v' — verify any traffic at all   ║"));
  Serial.println(F("║  3. Try swapping CANH ↔ CANL wires        ║"));
  Serial.println(F("║  4. Verify 120R switch on board is ON      ║"));
  Serial.println(F("║  5. Verify GND → heater 12V supply GND    ║"));
  Serial.println(F("╠════════════════════════════════════════════╣"));
  Serial.println(F("║  CONFIRMED PRESETS (type 'h <temp>'):      ║"));
  Serial.println(F("║  h 66  h 75  h 78  h 80  (all confirmed)  ║"));
  Serial.println(F("╠════════════════════════════════════════════╣"));
  Serial.println(F("║  FAULT CODES (0x2C4 D3 byte):              ║"));
  Serial.println(F("║  0x20 = Flame loss / fuel starvation       ║"));
  Serial.println(F("║  0x40 = Overtemperature / exhaust blocked  ║"));
  Serial.println(F("╠════════════════════════════════════════════╣"));
  Serial.println(F("║  WHAT TO WATCH FOR:                        ║"));
  Serial.println(F("║  0x2C4 D1=0x09 → heater heating           ║"));
  Serial.println(F("║  0x2C4 D1=0x0B → FAULT (type 'f')         ║"));
  Serial.println(F("║  0x2C4 D2=0x20 → flame confirmed          ║"));
  Serial.println(F("║  Counter sync  → command acknowledged     ║"));
  Serial.println(F("╚════════════════════════════════════════════╝\n"));
}

// ============================================================
//  SERIAL INPUT HANDLER
// ============================================================
String g_serial_buf = "";

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    // Emergency stop on 'E' or 'e' — no Enter needed
    if (c == 'E' || c == 'e') {
      emergencyStop();
      g_serial_buf = "";
      return;
    }

    if (c == '\r') continue;

    if (c == '\n') {
      g_serial_buf.trim();
      if (g_serial_buf.length() == 0) {
        g_serial_buf = "";
        return;
      }

      Serial.print(F("> "));
      Serial.println(g_serial_buf);

      char   cmd = g_serial_buf.charAt(0);
      String arg = (g_serial_buf.length() > 1) ? g_serial_buf.substring(2) : "";
      arg.trim();

      switch (cmd) {
        case 'h': case 'H': {
          if (arg.length() > 0) {
            int t = arg.toInt();
            if (!setSetpoint(t)) break;
          }
          setMode(MODE_HEAT);
          break;
        }

        case 'o': case 'O':
          setMode(MODE_IDLE);
          break;

        case 't': case 'T': {
          if (arg.length() == 0) {
            Serial.println(F("[ERR ] Usage: t <degrees_F>  e.g. t 78"));
          } else {
            setSetpoint(arg.toInt());
          }
          break;
        }

        case 's': case 'S':
          printStatus();
          break;

        case 'b': case 'B':
          printBusStatus();
          break;

        case 'r': case 'R':
          busRecover();
          break;

        case 'v': case 'V':
          g_verbose = !g_verbose;
          Serial.printf("[CFG ] Verbose mode: %s\n", g_verbose ? "ON" : "OFF");
          break;

        case 'm': case 'M':
          g_monitor = !g_monitor;
          Serial.printf("[CFG ] Startup monitor: %s\n", g_monitor ? "ON" : "OFF");
          break;

        case 'i': case 'I':
          Serial.println(F("[CMD ] Re-sending init burst..."));
          sendInitBurst();
          break;

        case 'f': case 'F': {
          // Fault status summary
          Serial.println(F("\n[FAUT] --- Fault Status ---"));
          if (!g_htr_fault_latch && g_htr_fault_code == HTR_FAULT_NONE) {
            Serial.println(F("[FAUT] No fault active. Heater OK."));
          } else {
            Serial.printf("[FAUT] Fault latch: %s\n", g_htr_fault_latch ? "SET" : "clear");
            Serial.printf("[FAUT] Fault code:  0x%02X\n", g_htr_fault_code);
            if (g_htr_fault_code & HTR_FAULT_FLAME) {
              Serial.println(F("[FAUT]   0x20 bit set → Flame Loss / Fuel Starvation"));
              Serial.println(F("[FAUT]   → Check: fuel line not kinked, tank not empty"));
            }
            if (g_htr_fault_code & HTR_FAULT_OVERTEMP) {
              Serial.println(F("[FAUT]   0x40 bit set → Overtemperature / Exhaust Blocked"));
              Serial.println(F("[FAUT]   → Check: exhaust outlet is clear"));
            }
            Serial.printf("[FAUT] Heater state: 0x%02X\n", g_htr_state);
            if (g_htr_state == HTR_STATE_IDLE && g_htr_fault_code != HTR_FAULT_NONE) {
              Serial.println(F("[FAUT] Post-fault clearing in progress (~26 s after fault)..."));
            }
            if (g_htr_fault_code == HTR_FAULT_NONE && g_htr_fault_latch) {
              Serial.println(F("[FAUT] Fault cleared — latch still set. Use 'r' to reset."));
            }
          }
          Serial.println(F("[FAUT] -------------------"));
          break;
        }

        case '?':
          printHelp();
          break;

        default:
          Serial.printf("[ERR ] Unknown command '%c'. Type ? for help.\n", cmd);
          break;
      }

      g_serial_buf = "";
    } else {
      g_serial_buf += c;
    }
  }
}

// ============================================================
//  SEND HEARTBEATS — Non-blocking timer-based dispatch
// ============================================================
void sendHeartbeats() {
  unsigned long now = millis();

  // 0x60D heartbeat at 100ms
  if (now - g_last_hb_sent >= INTERVAL_HEARTBEAT) {
    g_last_hb_sent = now;
    sendHeartbeat60D();
  }

  // 0x54 + 0x55/56/57 command group at 200ms
  if (now - g_last_cmd_sent >= INTERVAL_CMD) {
    g_last_cmd_sent = now;
    sendCmd54();
    sendCfg55_57();
  }

  // 0x65 periodic burst every 10s
  if (now - g_last_per_sent >= INTERVAL_PERIODIC) {
    g_last_per_sent = now;
    sendPeriodic65();
    g_cmd_ctr++;
    if (g_cmd_ctr == 0) g_cmd_ctr = 0x0100;
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

#ifdef USE_NEOPIXEL
  led.begin();
  led.setBrightness(LED_BRIGHTNESS);
  ledRed();
  delay(300);
  ledOff();
#endif

  Serial.println(F(""));
  Serial.println(F("╔═══════════════════════════════════════════════╗"));
  Serial.println(F("║   ESPAR GAS HEATER — CAN BUS TESTER          ║"));
  Serial.println(F("║   WeAct CAN485 DevBoard V1  (ESP32 onboard)  ║"));
  Serial.println(F("╚═══════════════════════════════════════════════╝"));
  Serial.printf("CAN TX : GPIO%d  (CA-IS2062A onboard)\n", (int)CAN_TX_PIN);
  Serial.printf("CAN RX : GPIO%d  (CA-IS2062A onboard)\n", (int)CAN_RX_PIN);
  Serial.printf("CAN Bus: 500 kbps\n");
  Serial.printf("Default: %d°F setpoint, starts in IDLE (OFF)\n", TEMP_DEFAULT_F);
  Serial.printf("Auto-off: %d min\n\n", AUTO_OFF_MIN);

  // ── Initialize TWAI (CAN) driver ──────────────────────────
#ifdef DIAG_LISTEN_ONLY
  Serial.println(F("*** DIAG: LISTEN-ONLY MODE — TX disabled, BUS OFF impossible ***"));
  Serial.println(F("*** If heater frames appear below, wiring is OK.              ***"));
  Serial.println(F("*** Re-comment #define DIAG_LISTEN_ONLY to restore control.   ***\n"));
  twai_general_config_t g_config =
    TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN,
                                (gpio_num_t)CAN_RX_PIN,
                                TWAI_MODE_LISTEN_ONLY);
#else
  twai_general_config_t g_config =
    TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN,
                                (gpio_num_t)CAN_RX_PIN,
                                TWAI_MODE_NORMAL);
#endif
  twai_timing_config_t t_config  = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config  = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  if (err != ESP_OK) {
    Serial.printf("[FAIL] TWAI driver install failed: 0x%04X\n", (unsigned)err);
    Serial.println(F("[FAIL] Check GPIO27/GPIO26 pin assignments."));
    ledRed();
    while (true) delay(1000);
  }

  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("[FAIL] TWAI start failed: 0x%04X\n", (unsigned)err);
    Serial.println(F("[FAIL] CAN bus may be shorted. Check CANH/CANL wiring."));
    ledRed();
    while (true) delay(1000);
  }

  g_can_ready = true;
  Serial.println(F("[OK  ] CAN driver started (GPIO27 TX, GPIO26 RX)"));
  ledBlue();

  // ── Settle briefly, then send init burst ─────────────────
  delay(200);
#ifdef DIAG_LISTEN_ONLY
  Serial.println(F("[DIAG] Listen-only — skipping init burst and all TX."));
  Serial.println(F("[DIAG] Watching for heater frames on 0x625 and 0x2C4 ..."));
#else
  sendInitBurst();
#endif

  // ── Initialize timers ────────────────────────────────────
  unsigned long now = millis();
  g_last_hb_sent  = now;
  g_last_cmd_sent = now;
  g_last_per_sent = now;

  // Send first 0x65 burst immediately
  sendPeriodic65();

  Serial.println(F("[OK  ] Controller online. Listening for heater..."));
  Serial.println(F(""));
  Serial.printf("[MON ] Startup monitor ON — printing ALL received frames for %ds.\n",
                STARTUP_MONITOR_SECS);
  Serial.println(F("[MON ] Heater should appear within 2 seconds of heater power-on."));
  Serial.println(F("[MON ] If nothing appears, type 'b' for bus diagnostics."));
  Serial.println(F("[MON ] Type 'm' to toggle monitor, 'v' for full verbose, '?' for help.\n"));
  ledBlue();
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  handleSerial();      // Process serial commands (non-blocking)
  receiveMessages();   // Poll incoming CAN frames
#ifndef DIAG_LISTEN_ONLY
  sendHeartbeats();    // Timer-driven TX (suppressed in listen-only mode)
#endif
  checkWatchdog();     // Safety watchdog + auto-off timer
}
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       