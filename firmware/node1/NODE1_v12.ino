/*
 * =====================================================================
 * PLANT HUB PRO - NODE1 v1.2 (ESP32 Core 3.x Compatible)
 * =====================================================================
 *
 * Remote moisture sensor node - ESP-NOW to NODE0, deep sleep between reads.
 *
 * HARDWARE:
 *   - ESP32 dev board
 *   - Capacitive moisture sensor -> GPIO 34
 *   - 3.7V LiPo battery
 *
 * SETUP:
 *   1. Get NODE0's MAC from its Serial Monitor at boot
 *   2. Replace node0Address below
 *   3. Calibrate dryVolts / wetVolts for your sensor
 *   4. Upload and power on
 *
 * VERSION: 1.2
 * DATE: 2026-02-11
 * =====================================================================
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>

/* ================= CONFIGURATION ================= */

const char* WIFI_SSID = "HomeTeach-IoT";
const char* WIFI_PASS = "hometeach";

#define SLEEP_DURATION    30 * 60 * 1000000ULL   // 30 minutes
#define MOISTURE_PIN      34                      // GPIO 34 (ADC1_CH6)
#define ZONE_ID           1

// Moisture sensor calibration
const float dryVolts = 2.691;   // Voltage in dry air
const float wetVolts = 0.971;   // Voltage in water

// *** REPLACE WITH YOUR NODE0's ACTUAL MAC ADDRESS ***
uint8_t node0Address[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ================= SHARED DATA STRUCTURE ================= */
// CRITICAL: Must be IDENTICAL to NODE0's RemoteSensorData
typedef struct __attribute__((packed)) RemoteSensorData {
  uint8_t zone;              // 1 byte
  int moisture;              // 4 bytes
  float voltage;             // 4 bytes
  float batteryVoltage;      // 4 bytes
  unsigned long timestamp;   // 4 bytes
} RemoteSensorData;          // Total: 17 bytes (packed)

RemoteSensorData sensorData;

/* ================= SEND CALLBACK ================= */
volatile bool sendComplete = false;
volatile bool sendSuccess = false;

// ESP32 Arduino core 3.x callback signature
void onDataSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  sendSuccess = (status == ESP_NOW_SEND_SUCCESS);
  sendComplete = true;
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  delay(100);

  analogSetAttenuation(ADC_11db);

  Serial.println("\n========================================");
  Serial.println("  PLANT HUB PRO - NODE1 v1.2");
  Serial.printf("  Zone: %d  |  Sleep: %d min\n", ZONE_ID, (int)(SLEEP_DURATION / 60000000));
  Serial.printf("  Struct size: %d bytes\n", sizeof(RemoteSensorData));
  Serial.println("========================================\n");

  readSensor();
  sendData();

  Serial.println("\nEntering deep sleep...");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION);
  esp_deep_sleep_start();
}

/* ================= READ SENSOR ================= */
void readSensor() {
  int rawADC = analogRead(MOISTURE_PIN);
  float voltage = (rawADC / 4095.0) * 3.3;

  int moisturePct = map(voltage * 1000,
                        wetVolts * 1000,
                        dryVolts * 1000,
                        100, 0);
  moisturePct = constrain(moisturePct, 0, 100);

  // Real battery voltage via 2x 10K voltage divider on GPIO 35
  // Battery(+) --[10K]--+--[10K]-- GND, midpoint to GPIO 35
  int battADC = analogRead(35);
  float battVoltage = (battADC / 4095.0) * 3.3 * 2.0;

  sensorData.zone           = ZONE_ID;
  sensorData.moisture       = moisturePct;
  sensorData.voltage        = voltage;
  sensorData.batteryVoltage = battVoltage;
  sensorData.timestamp      = millis();

  Serial.printf("  ADC: %d  Voltage: %.3fV  Moisture: %d%%  Battery: %.2fV\n",
                rawADC, voltage, moisturePct, battVoltage);
}

/* ================= SEND DATA VIA ESP-NOW ================= */
void sendData() {
  // Connect to WiFi to sync to the same channel as NODE0
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("  WiFi connected - channel %d\n", WiFi.channel());
  } else {
    Serial.println("  WiFi timeout - will try sending anyway");
  }

  // Disconnect WiFi but keep radio on same channel
  WiFi.disconnect(false);

  Serial.printf("  MAC: %s\n", WiFi.macAddress().c_str());

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("  ERROR: ESP-NOW init failed");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  esp_now_register_send_cb(onDataSent);

  // Add NODE0 as peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, node0Address, 6);
  peerInfo.channel = 0;       // 0 = use current channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("  ERROR: Failed to add NODE0 as peer");
    Serial.println("  Check that node0Address MAC is correct!");
    esp_now_deinit();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  // Send with retries
  for (int attempt = 1; attempt <= 3; attempt++) {
    sendComplete = false;
    sendSuccess = false;

    esp_err_t result = esp_now_send(node0Address,
                                    (uint8_t *)&sensorData,
                                    sizeof(sensorData));

    if (result != ESP_OK) {
      Serial.printf("  Attempt %d: queue error %d\n", attempt, result);
      delay(100);
      continue;
    }

    // Wait for delivery callback
    unsigned long start = millis();
    while (!sendComplete && (millis() - start) < 1000) {
      delay(1);
    }

    if (sendSuccess) {
      Serial.printf("  DELIVERED on attempt %d (%d bytes)\n", attempt, sizeof(sensorData));
      break;
    } else if (sendComplete) {
      Serial.printf("  Attempt %d: no ACK from NODE0\n", attempt);
    } else {
      Serial.printf("  Attempt %d: timed out\n", attempt);
    }
    delay(100);
  }

  // Cleanup
  esp_now_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("  Radio off");
}

/* ================= LOOP ================= */
void loop() {
  // Never reached - deep sleep restarts from setup()
}
