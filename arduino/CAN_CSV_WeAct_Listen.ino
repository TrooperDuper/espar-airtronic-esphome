/* ESP32 TWAI CAN Logger — WeAct CAN485
   Outputs SavvyCAN-compatible CSV over Serial.
   
   Wiring (WeAct CAN485 V1.0 — confirmed from schematic):
     RX_PIN = GPIO26 (CAN_RXD)
     TX_PIN = GPIO27 (CAN_TXD)
   
   Serial monitor: 115200 baud
   Copy/paste output directly into a .csv file and open in SavvyCAN.
   
   CSV format matches SavvyCAN export:
     Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8
*/

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "driver/twai.h"

// Pins used to connect to CAN bus transceiver:
#define RX_PIN 26
#define TX_PIN 27

// Polling interval for alert checks
#define POLLING_RATE_MS 1000

static bool driver_installed = false;
static uint32_t start_time_ms = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  // Initialize configuration structures
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
    (gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  // Install TWAI driver
  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
    Serial.println("# ERROR: Failed to install TWAI driver");
    return;
  }

  // Start TWAI driver
  if (twai_start() != ESP_OK) {
    Serial.println("# ERROR: Failed to start TWAI driver");
    return;
  }

  // Enable alerts
  uint32_t alerts_to_enable = TWAI_ALERT_RX_DATA | TWAI_ALERT_ERR_PASS |
                               TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_QUEUE_FULL;
  if (twai_reconfigure_alerts(alerts_to_enable, NULL) != ESP_OK) {
    Serial.println("# ERROR: Failed to configure alerts");
    return;
  }

  driver_installed = true;
  start_time_ms = millis();

  // Print CSV header — SavvyCAN compatible
  Serial.println("Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8");
}

static void print_csv_frame(twai_message_t &message) {
  // Time stamp in microseconds relative to start
  uint32_t ts = (millis() - start_time_ms) * 1000;

  // ID zero-padded to 8 hex digits
  // Extended flag
  const char* ext = message.extd ? "true" : "false";

  // Build data bytes — pad to 8 bytes, empty if beyond DLC
  char d[8][3];
  for (int i = 0; i < 8; i++) {
    if (i < message.data_length_code && !message.rtr) {
      snprintf(d[i], sizeof(d[i]), "%02X", message.data[i]);
    } else {
      d[i][0] = '\0';
    }
  }

  Serial.printf("%lu,%08lX,%s,Rx,0,%d,%s,%s,%s,%s,%s,%s,%s,%s,\n",
    (unsigned long)ts,
    (unsigned long)message.identifier,
    ext,
    message.data_length_code,
    d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]
  );
}

void loop() {
  if (!driver_installed) {
    delay(1000);
    return;
  }

  uint32_t alerts_triggered;
  twai_read_alerts(&alerts_triggered, pdMS_TO_TICKS(POLLING_RATE_MS));
  twai_status_info_t twaistatus;
  twai_get_status_info(&twaistatus);

  // Log alerts as comments (# prefix — SavvyCAN ignores non-CSV lines)
  if (alerts_triggered & TWAI_ALERT_ERR_PASS) {
    Serial.printf("# Alert: TWAI error passive. Bus errors: %lu\n",
      twaistatus.bus_error_count);
  }
  if (alerts_triggered & TWAI_ALERT_BUS_ERROR) {
    Serial.printf("# Alert: Bus error. Count: %lu\n",
      twaistatus.bus_error_count);
  }
  if (alerts_triggered & TWAI_ALERT_RX_QUEUE_FULL) {
    Serial.printf("# Alert: RX queue full. Buffered: %lu  Missed: %lu  Overrun: %lu\n",
      twaistatus.msgs_to_rx,
      twaistatus.rx_missed_count,
      twaistatus.rx_overrun_count);
  }

  // Drain all received frames
  if (alerts_triggered & TWAI_ALERT_RX_DATA) {
    twai_message_t message;
    while (twai_receive(&message, 0) == ESP_OK) {
      print_csv_frame(message);
    }
  }
}
