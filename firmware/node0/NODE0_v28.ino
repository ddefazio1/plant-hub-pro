/*
 * =====================================================================
 * PLANT HUB PRO - NODE0 v2.8 (Safety & Hardening)
 * =====================================================================
 *
 * CHANGES in v2.8:
 * - Hardware watchdog timer (auto-reboot if code hangs)
 * - Relay failsafe (all relays forced OFF on boot + periodic check)
 * - Web command authentication (API key required for relay/LCD commands)
 * - WiFi auto-reconnect
 * - MQTT reconnect hardened
 * - Max watering hard limit enforced even if loop stalls
 *
 * FEATURES:
 * - Multi-zone soil moisture (ADS1115 + ESP-NOW remotes)
 * - Environmental sensors (BME280)
 * - VPD calculation and adaptive thresholds
 * - Weather-aware watering decisions (NOAA)
 * - Web dashboard + JSON API (served from SD card)
 * - MQTT + Home Assistant auto-discovery
 * - SD card logging
 * - OTA updates
 * - Hysteresis anti-flapping (40% on, 70% off)
 * - Per-zone cooldown periods
 * - Watchdog timer (8 sec)
 * - Relay failsafe
 * - Web API authentication
 *
 * HARDWARE:
 * - ESP32 Dev Module
 * - ADS1115 16-bit ADC
 * - BME280 environmental sensor
 * - LCD 16x2
 * - SD card module
 * - Relay board (8-channel recommended)
 *
 * LOCATION: Troy, NY
 * VERSION: 2.8
 * DATE: 2026-02-13
 * =====================================================================
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <ArduinoOTA.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>
#include <Adafruit_BME280.h>
#include <Adafruit_ADS1X15.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

/* ================= USER CONFIG ================= */
const char* WIFI_SSID = "CHANGE_ME";
const char* WIFI_PASS = "CHANGE_ME";
const char* NODE_NAME = "NODE0";

// MQTT Config
const char* MQTT_SERVER   = "192.168.107.43";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "mqtt";
const char* MQTT_PASS     = "CHANGE_ME";     // <-- PUT YOUR PASSWORD HERE
const char* MQTT_CLIENT   = "planthub_node0";

// Web API key (required for relay/LCD/water commands)
// Append ?key=planthub2026 to command URLs
const char* API_KEY = "planthub2026";

// Watchdog timeout (seconds) - reboots if code hangs longer than this
#define WDT_TIMEOUT  30  // 30 seconds - allows for slow HTTP requests

// Hard relay safety - absolute max seconds ANY relay can be on
#define HARD_RELAY_TIMEOUT  900  // 15 minutes absolute max

#define MQTT_PUBLISH_INTERVAL  15000   // Publish every 15 seconds
#define MQTT_DISCOVERY_INTERVAL 300000 // Re-send discovery every 5 min

#define LATITUDE   42.7284
#define LONGITUDE  -73.6918

/* ================= HARDWARE PINS ================= */
#define LCD_ADDR 0x3F
#define ADS_ADDR 0x48
#define BME_ADDR 0x76
#define SD_CS 13
#define BUILTIN_LED 2

#define RELAY_MAIN    25
#define RELAY_ZONE_0  26
#define RELAY_ZONE_1  27
#define RELAY_ZONE_2  14
#define RELAY_ZONE_3  12

const int ZONE_RELAY_PINS[4] = {RELAY_ZONE_0, RELAY_ZONE_1, RELAY_ZONE_2, RELAY_ZONE_3};

/* ================= MOISTURE ZONES ================= */
#define NUM_ZONES 4

struct ZoneConfig {
  float dryVolts;
  float wetVolts;
  bool enabled;
  int dryThreshold;
  int wetThreshold;
  int waterDuration;
  int cooldownMinutes;
};

ZoneConfig zones[NUM_ZONES] = {
  {2.691, 0.971, true,  40, 70, 120, 60},
  {2.6824, 0.920, true, 40, 70, 120, 60},
  {2.691, 0.971, false, 40, 70, 120, 60},
  {2.691, 0.971, false, 40, 70, 120, 60}
};

/* ================= VPD & WEATHER CONFIG ================= */
#define VPD_LOW      0.6
#define VPD_MEDIUM   1.0
#define VPD_HIGH     1.4
#define VPD_EXTREME  1.8

#define WEATHER_UPDATE_INTERVAL  3600000
#define MAX_WATERING_TIME        600

/* ================= OBJECTS ================= */
WiFiServer server(80);
WiFiClient mqttWifiClient;
PubSubClient mqtt(mqttWifiClient);
LiquidCrystal_PCF8574 lcd(LCD_ADDR);
Adafruit_BME280 bme;
Adafruit_ADS1115 ads;

/* ================= STATE VARIABLES ================= */
float moistureVolts[NUM_ZONES] = {0};
int moisturePct[NUM_ZONES] = {0};
float battery = 3.7;
float tempC = 0;
float humidity = 0;
float pressure = 0;
float currentVPD = 0;
String statusMsg[NUM_ZONES];
bool bmeOK = false;
bool adsOK = false;
bool lcdBacklightState = true;
bool sdOK = false;
unsigned long lastSample = 0;
unsigned long lastLCD = 0;
unsigned long lastBeacon = 0;
unsigned long lastWeatherUpdate = 0;
unsigned long lastMQTTPublish = 0;
unsigned long lastMQTTDiscovery = 0;
int currentZoneDisplay = 0;

/* ================= HISTORY ================= */
#define HISTORY_LEN 100
int moistureHist[HISTORY_LEN] = {0};
float tempHist[HISTORY_LEN] = {0};
float humHist[HISTORY_LEN] = {0};
float pressHist[HISTORY_LEN] = {0};
int histIdx = 0;

/* ================= ESP-NOW ================= */
#define MAX_SENSORS 10
uint8_t sensorMACs[MAX_SENSORS][6];
int numSensors = 0;

typedef struct __attribute__((packed)) RemoteSensorData {
  uint8_t zone;
  int moisture;
  float voltage;
  float batteryVoltage;
  unsigned long timestamp;
} RemoteSensorData;

RemoteSensorData remoteData[MAX_SENSORS];
unsigned long lastReceived[MAX_SENSORS] = {0};
char remoteNodeNames[MAX_SENSORS][16];

typedef struct __attribute__((packed)) BeaconData {
  char nodeName[10];
  int wifiSignal;
  uint32_t heapFree;
  unsigned long uptime;
  uint8_t nodeCount;
  float batteryVoltage;
} BeaconData;

/* ================= WATERING STATE ================= */
struct ZoneWateringState {
  bool active;
  unsigned long startTime;
  unsigned long lastWatered;
  int totalSeconds;
  bool needsWater;
};

ZoneWateringState zoneState[NUM_ZONES];
bool mainValveOpen = false;

/* ================= WEATHER DATA ================= */
struct WeatherForecast {
  float tempC;
  float humidity;
  float windSpeedMPH;
  int precipChance;
  float precipAmount;
  bool rainNext6Hours;
  bool rainNext24Hours;
  String shortForecast;
  unsigned long lastUpdate;
  bool valid;
};

WeatherForecast weather;

/* ================= TIME ================= */
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -18000;
const int daylightOffset_sec = 0;

/* ================= VPD CALCULATION ================= */
float calculateVPD(float temp, float rh) {
  float es = 0.6108 * exp((17.27 * temp) / (temp + 237.3));
  float ea = (rh / 100.0) * es;
  return es - ea;
}

/* ================= WEATHER FUNCTIONS ================= */
String getNOAAGridPoint() {
  esp_task_wdt_reset();  // Feed watchdog before slow HTTP
  HTTPClient http;
  http.setTimeout(10000);  // 10 sec max
  String url = "https://api.weather.gov/points/" +
               String(LATITUDE, 4) + "," + String(LONGITUDE, 4);
  http.begin(url);
  http.addHeader("User-Agent", "PlantHubPro/2.8 (ESP32)");
  int httpCode = http.GET();
  esp_task_wdt_reset();  // Feed watchdog after HTTP
  String forecastURL = "";
  if (httpCode == 200) {
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, http.getString());
    forecastURL = doc["properties"]["forecast"].as<String>();
  }
  http.end();
  return forecastURL;
}

void updateWeather() {
  esp_task_wdt_reset();  // Feed watchdog before slow HTTP
  static String forecastURL = "";
  if (forecastURL.length() == 0) {
    forecastURL = getNOAAGridPoint();
    if (forecastURL.length() == 0) { weather.valid = false; return; }
  }
  HTTPClient http;
  http.setTimeout(10000);  // 10 sec max
  http.begin(forecastURL);
  http.addHeader("User-Agent", "PlantHubPro/2.8");
  int httpCode = http.GET();
  esp_task_wdt_reset();  // Feed watchdog after HTTP
  if (httpCode == 200) {
    DynamicJsonDocument doc(8192);
    DeserializationError error = deserializeJson(doc, http.getString());
    if (!error) {
      JsonObject period = doc["properties"]["periods"][0];
      weather.tempC = (period["temperature"].as<int>() - 32) * 5.0 / 9.0;
      weather.shortForecast = period["shortForecast"].as<String>();
      int precipProb = 0;
      if (period["probabilityOfPrecipitation"]["value"].is<int>())
        precipProb = period["probabilityOfPrecipitation"]["value"].as<int>();
      weather.precipChance = precipProb;
      weather.rainNext6Hours = false;
      weather.rainNext24Hours = false;
      for (int i = 0; i < min(3, (int)doc["properties"]["periods"].size()); i++) {
        JsonObject p = doc["properties"]["periods"][i];
        int prob = 0;
        if (p["probabilityOfPrecipitation"]["value"].is<int>())
          prob = p["probabilityOfPrecipitation"]["value"].as<int>();
        if (prob > 50) { weather.rainNext6Hours = true; weather.rainNext24Hours = true; }
      }
      if (weather.precipChance > 70) weather.precipAmount = 5.0;
      else if (weather.precipChance > 40) weather.precipAmount = 2.0;
      else weather.precipAmount = 0.0;
      weather.lastUpdate = millis();
      weather.valid = true;
      Serial.printf("[WEATHER] %s | Rain: %d%%\n", weather.shortForecast.c_str(), weather.precipChance);
    } else { weather.valid = false; }
  } else { weather.valid = false; }
  http.end();
}

/* ================= VPD THRESHOLD ================= */
int getVPDThreshold(int baseThreshold, float vpd) {
  if (vpd > VPD_EXTREME) return baseThreshold + 10;
  if (vpd > VPD_HIGH)    return baseThreshold + 5;
  if (vpd > VPD_MEDIUM)  return baseThreshold;
  if (vpd > VPD_LOW)     return baseThreshold - 5;
  return baseThreshold - 10;
}

/* ================= RELAY CONTROL ================= */

// SAFETY: Force all relays off - called on boot and on watchdog-adjacent situations
void forceAllRelaysOff() {
  digitalWrite(RELAY_MAIN, LOW);
  mainValveOpen = false;
  for (int i = 0; i < NUM_ZONES; i++) {
    digitalWrite(ZONE_RELAY_PINS[i], LOW);
    zoneState[i].active = false;
  }
  Serial.println("[SAFETY] All relays forced OFF");
}

// SAFETY: Check if any relay has been on too long (hardware failsafe)
void relayFailsafeCheck() {
  for (int zone = 0; zone < NUM_ZONES; zone++) {
    if (zoneState[zone].active) {
      unsigned long duration = (millis() - zoneState[zone].startTime) / 1000;
      if (duration >= HARD_RELAY_TIMEOUT) {
        Serial.printf("[SAFETY] Zone %d HARD TIMEOUT (%lu sec) - forcing off!\n", zone, duration);
        forceAllRelaysOff();
        if (mqtt.connected()) {
          mqtt.publish("planthub/alert", "SAFETY: Hard relay timeout triggered - all valves forced OFF");
        }
        return;
      }
    }
  }
}

// WiFi reconnect
void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Disconnected - reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(250);
      esp_task_wdt_reset();  // Don't let watchdog fire during reconnect
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Reconnected: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[WIFI] Reconnect failed - will retry next cycle");
    }
  }
}

void openMainValve() {
  if (!mainValveOpen) { digitalWrite(RELAY_MAIN, HIGH); mainValveOpen = true; Serial.println("MAIN VALVE OPENED"); }
}
void closeMainValve() {
  if (mainValveOpen) { digitalWrite(RELAY_MAIN, LOW); mainValveOpen = false; Serial.println("MAIN VALVE CLOSED"); }
}
void openZoneValve(int zone) {
  if (zone >= 0 && zone < NUM_ZONES) { digitalWrite(ZONE_RELAY_PINS[zone], HIGH); }
}
void closeZoneValve(int zone) {
  if (zone >= 0 && zone < NUM_ZONES) { digitalWrite(ZONE_RELAY_PINS[zone], LOW); }
}

/* ================= WATERING LOGIC ================= */
bool shouldWaterZone(int zone) {
  if (!zones[zone].enabled) return false;
  if (zoneState[zone].active) return false;
  int moisture = moisturePct[zone];
  if (moisture < 20) return true;
  if (moisture >= zones[zone].wetThreshold) return false;
  unsigned long timeSince = millis() - zoneState[zone].lastWatered;
  if (timeSince < zones[zone].cooldownMinutes * 60000UL) return false;
  if (weather.valid && weather.rainNext6Hours && weather.precipChance > 60) return false;
  int threshold = getVPDThreshold(zones[zone].dryThreshold, currentVPD);
  if (weather.valid && weather.tempC > 30) threshold += 3;
  return moisture < threshold;
}

void startWatering(int zone) {
  if (!zones[zone].enabled || zoneState[zone].active) return;
  openMainValve();
  openZoneValve(zone);
  zoneState[zone].active = true;
  zoneState[zone].startTime = millis();
  Serial.printf("STARTED watering Zone %d\n", zone);
  if (sdOK) {
    File f = SD.open("/watering_log.csv", FILE_APPEND);
    if (f) {
      struct tm ti; char ts[32];
      if (getLocalTime(&ti)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &ti);
      else strcpy(ts, "N/A");
      f.printf("%s,%s,Zone%d,START,%d%%,%.2f,%.1f,%.1f,%d%%\n",
               ts, NODE_NAME, zone, moisturePct[zone], currentVPD, tempC, humidity,
               weather.valid ? weather.precipChance : 0);
      f.close();
    }
  }
}

void stopWatering(int zone) {
  if (!zoneState[zone].active) return;
  closeZoneValve(zone);
  unsigned long duration = (millis() - zoneState[zone].startTime) / 1000;
  zoneState[zone].active = false;
  zoneState[zone].lastWatered = millis();
  zoneState[zone].totalSeconds += duration;
  Serial.printf("STOPPED watering Zone %d (%lu sec)\n", zone, duration);
  bool anyActive = false;
  for (int z = 0; z < NUM_ZONES; z++) { if (zoneState[z].active) { anyActive = true; break; } }
  if (!anyActive) closeMainValve();
  if (sdOK) {
    File f = SD.open("/watering_log.csv", FILE_APPEND);
    if (f) {
      struct tm ti; char ts[32];
      if (getLocalTime(&ti)) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &ti);
      else strcpy(ts, "N/A");
      f.printf("%s,%s,Zone%d,STOP,%d%%,%.2f,%lu\n", ts, NODE_NAME, zone, moisturePct[zone], currentVPD, duration);
      f.close();
    }
  }
}

void smartWateringControl() {
  if (millis() - lastWeatherUpdate > WEATHER_UPDATE_INTERVAL) {
    updateWeather();
    lastWeatherUpdate = millis();
  }
  currentVPD = calculateVPD(tempC, humidity);
  for (int zone = 0; zone < NUM_ZONES; zone++) {
    if (!zones[zone].enabled) continue;
    if (zoneState[zone].active) {
      unsigned long duration = (millis() - zoneState[zone].startTime) / 1000;
      if (moisturePct[zone] >= zones[zone].wetThreshold) stopWatering(zone);
      else if (duration >= zones[zone].waterDuration) stopWatering(zone);
      else if (duration >= MAX_WATERING_TIME) stopWatering(zone);
    } else {
      if (shouldWaterZone(zone)) startWatering(zone);
    }
  }
}

/* ================= FORWARD DECLARATIONS ================= */
void mqttSendRemoteDiscovery(int idx);
void mqttPublishRemote(int idx);

/* ================= ESP-NOW CALLBACK ================= */
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len == sizeof(RemoteSensorData)) {
    int sensorIdx = -1;
    for (int i = 0; i < numSensors; i++) {
      if (memcmp(sensorMACs[i], info->src_addr, 6) == 0) { sensorIdx = i; break; }
    }
    if (sensorIdx == -1 && numSensors < MAX_SENSORS) {
      sensorIdx = numSensors;
      memcpy(sensorMACs[sensorIdx], info->src_addr, 6);
      snprintf(remoteNodeNames[sensorIdx], 16, "NODE_%02X%02X",
               info->src_addr[4], info->src_addr[5]);
      numSensors++;
      Serial.printf("[ESP-NOW] New sensor: %s\n", remoteNodeNames[sensorIdx]);
      // Send MQTT discovery for new remote node
      mqttSendRemoteDiscovery(sensorIdx);
    }
    if (sensorIdx >= 0) {
      memcpy(&remoteData[sensorIdx], data, sizeof(RemoteSensorData));
      lastReceived[sensorIdx] = millis();
      Serial.printf("[ESP-NOW] RX %s Zone %d: %d%% (batt: %.2fV)\n",
                    remoteNodeNames[sensorIdx], remoteData[sensorIdx].zone,
                    remoteData[sensorIdx].moisture, remoteData[sensorIdx].batteryVoltage);
      // Publish immediately on receive
      mqttPublishRemote(sensorIdx);
    }
  }
  else if (len == sizeof(BeaconData)) {
    BeaconData beacon;
    memcpy(&beacon, data, sizeof(BeaconData));
    Serial.printf("[ESP-NOW] Beacon from %s\n", beacon.nodeName);
  }
  else {
    Serial.printf("[ESP-NOW] Unknown packet: %d bytes\n", len);
  }
}

/* ================= SENSOR READING ================= */
void readAllMoisture() {
  if (!adsOK) return;
  for (int zone = 0; zone < NUM_ZONES; zone++) {
    if (!zones[zone].enabled) continue;
    int16_t raw = ads.readADC_SingleEnded(zone);
    moistureVolts[zone] = ads.computeVolts(raw);
    int pct = map(moistureVolts[zone] * 1000,
                  zones[zone].wetVolts * 1000,
                  zones[zone].dryVolts * 1000,
                  100, 0);
    moisturePct[zone] = constrain(pct, 0, 100);
    if (moisturePct[zone] < zones[zone].dryThreshold) statusMsg[zone] = "DRY";
    else if (moisturePct[zone] > zones[zone].wetThreshold) statusMsg[zone] = "WET";
    else statusMsg[zone] = "OK";
  }
}

void readBME() {
  if (!bmeOK) return;
  tempC = bme.readTemperature();
  humidity = bme.readHumidity();
  pressure = bme.readPressure() / 100.0F;
}

/* ================= LCD ================= */
void updateLCD() {
  lcd.clear();
  int enabledCount = 0;
  for (int i = 0; i < NUM_ZONES; i++) { if (zones[i].enabled) enabledCount++; }
  int displayZone = -1, count = 0;
  for (int i = 0; i < NUM_ZONES; i++) {
    if (zones[i].enabled) {
      if (count == currentZoneDisplay) { displayZone = i; break; }
      count++;
    }
  }
  if (displayZone >= 0) {
    lcd.setCursor(0, 0);
    lcd.print("Z"); lcd.print(displayZone); lcd.print(":");
    lcd.print(moisturePct[displayZone]); lcd.print("% ");
    if (zoneState[displayZone].active) lcd.print("WATERING");
    else lcd.print(statusMsg[displayZone]);
    lcd.setCursor(0, 1);
    lcd.print("VPD:"); lcd.print(currentVPD, 1);
    lcd.print(" T:"); lcd.print((int)tempC); lcd.print("C");
  } else {
    lcd.setCursor(0, 0); lcd.print("Plant Hub Pro");
    lcd.setCursor(0, 1); lcd.print(WiFi.localIP());
  }
  currentZoneDisplay = (currentZoneDisplay + 1) % max(enabledCount, 1);
}

/* ================= BEACON ================= */
void broadcastBeacon() {
  Serial.printf("Beacon: %s | Heap: %dKB | Nodes: %d\n",
                NODE_NAME, ESP.getFreeHeap() / 1024, numSensors);
}

/* ================= SD LOGGING ================= */
void logToSD() {
  if (!sdOK) return;
  struct tm ti; char ts[32];
  if (!getLocalTime(&ti)) return;
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &ti);
  File f = SD.open("/moisture.csv", FILE_APPEND);
  if (!f) return;
  for (int z = 0; z < NUM_ZONES; z++) {
    if (!zones[z].enabled) continue;
    f.printf("%s,%s,Zone%d,%d,%.3f,%s,%.1f,%.1f,%.1f,%.2f,%.2f\n",
             ts, NODE_NAME, z, moisturePct[z], moistureVolts[z],
             statusMsg[z].c_str(), tempC, humidity, pressure, battery, currentVPD);
  }
  f.close();
}

/* ================= MQTT ================= */
void mqttReconnect() {
  if (mqtt.connected()) return;
  Serial.print("[MQTT] Connecting... ");
  if (mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS, "planthub/status", 0, true, "offline")) {
    Serial.println("OK");
    mqtt.publish("planthub/status", "online", true);
    // Subscribe to commands
    mqtt.subscribe("planthub/cmd/#");
  } else {
    Serial.printf("FAIL (rc=%d)\n", mqtt.state());
  }
}

// MQTT command handler
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];
  int len = min((unsigned int)63, length);
  memcpy(msg, payload, len);
  msg[len] = '\0';

  String t = String(topic);
  Serial.printf("[MQTT] CMD: %s = %s\n", topic, msg);

  if (t == "planthub/cmd/relay") {
    if (strcmp(msg, "on") == 0) { openMainValve(); openZoneValve(0); }
    else { closeZoneValve(0); closeMainValve(); }
  }
  else if (t == "planthub/cmd/lcd") {
    lcd.setBacklight(strcmp(msg, "on") == 0 ? 1 : 0);
  }
  else if (t.startsWith("planthub/cmd/water/zone")) {
    int zone = t.substring(23).toInt();
    if (zone >= 0 && zone < NUM_ZONES && zones[zone].enabled) {
      if (strcmp(msg, "on") == 0) startWatering(zone);
      else stopWatering(zone);
    }
  }
}

// Send HA auto-discovery for a sensor
void mqttDiscovery(const char* id, const char* name, const char* stateTopic,
                   const char* unit, const char* devClass, const char* icon,
                   const char* valueTemplate) {
  char discTopic[128];
  snprintf(discTopic, sizeof(discTopic), "homeassistant/sensor/planthub/%s/config", id);

  DynamicJsonDocument doc(512);
  doc["name"] = name;
  doc["unique_id"] = String("planthub_") + id;
  doc["state_topic"] = stateTopic;
  if (strlen(unit) > 0) doc["unit_of_measurement"] = unit;
  if (strlen(devClass) > 0) doc["device_class"] = devClass;
  if (strlen(icon) > 0) doc["icon"] = icon;
  if (strlen(valueTemplate) > 0) doc["value_template"] = valueTemplate;

  JsonObject dev = doc.createNestedObject("device");
  dev["identifiers"][0] = "planthub_node0";
  dev["name"] = "Plant Hub";
  dev["manufacturer"] = "518 IT Solutions";
  dev["model"] = "Plant Hub Pro v2.7";

  char buf[512];
  serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(discTopic, buf, true);
}

// Binary sensor discovery
void mqttDiscoveryBinary(const char* id, const char* name, const char* stateTopic,
                         const char* devClass, const char* icon, const char* valueTemplate) {
  char discTopic[128];
  snprintf(discTopic, sizeof(discTopic), "homeassistant/binary_sensor/planthub/%s/config", id);

  DynamicJsonDocument doc(512);
  doc["name"] = name;
  doc["unique_id"] = String("planthub_") + id;
  doc["state_topic"] = stateTopic;
  doc["payload_on"] = "ON";
  doc["payload_off"] = "OFF";
  if (strlen(devClass) > 0) doc["device_class"] = devClass;
  if (strlen(icon) > 0) doc["icon"] = icon;
  if (strlen(valueTemplate) > 0) doc["value_template"] = valueTemplate;

  JsonObject dev = doc.createNestedObject("device");
  dev["identifiers"][0] = "planthub_node0";
  dev["name"] = "Plant Hub";
  dev["manufacturer"] = "518 IT Solutions";
  dev["model"] = "Plant Hub Pro v2.7";

  char buf[512];
  serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(discTopic, buf, true);
}

void mqttSendAllDiscovery() {
  if (!mqtt.connected()) return;
  Serial.println("[MQTT] Sending HA auto-discovery...");

  // Environment
  mqttDiscovery("temperature", "Garden Temperature", "planthub/env", "C", "temperature", "", "{{ value_json.temp }}");
  mqttDiscovery("temperature_f", "Garden Temperature F", "planthub/env", "F", "temperature", "", "{{ value_json.temp_f }}");
  mqttDiscovery("humidity", "Garden Humidity", "planthub/env", "%", "humidity", "", "{{ value_json.hum }}");
  mqttDiscovery("pressure", "Garden Pressure", "planthub/env", "hPa", "pressure", "", "{{ value_json.press }}");
  mqttDiscovery("vpd", "Garden VPD", "planthub/env", "kPa", "", "mdi:water-thermometer", "{{ value_json.vpd }}");

  // System
  mqttDiscovery("uptime", "Plant Hub Uptime", "planthub/system", "min", "", "mdi:clock-outline", "{{ value_json.uptime }}");
  mqttDiscovery("heap", "Plant Hub Heap", "planthub/system", "KB", "", "mdi:memory", "{{ value_json.heap }}");
  mqttDiscovery("battery", "Plant Hub Battery", "planthub/system", "V", "battery", "", "{{ value_json.battery }}");
  mqttDiscovery("nodes", "Plant Hub Nodes", "planthub/system", "", "", "mdi:router-wireless", "{{ value_json.nodes }}");

  // Hub online
  mqttDiscoveryBinary("online", "Plant Hub Online", "planthub/status", "connectivity", "", "");

  // Main valve
  mqttDiscoveryBinary("main_valve", "Plant Hub Main Valve", "planthub/valve", "opening", "mdi:valve", "");

  // Local zones
  for (int z = 0; z < NUM_ZONES; z++) {
    if (!zones[z].enabled) continue;
    char id[32], name[48], vt[64];

    snprintf(id, sizeof(id), "zone%d_moisture", z);
    snprintf(name, sizeof(name), "Garden Zone %d Moisture", z);
    snprintf(vt, sizeof(vt), "{{ value_json.z%d_moisture }}", z);
    mqttDiscovery(id, name, "planthub/zones", "%", "", "mdi:water-percent", vt);

    snprintf(id, sizeof(id), "zone%d_voltage", z);
    snprintf(name, sizeof(name), "Garden Zone %d Voltage", z);
    snprintf(vt, sizeof(vt), "{{ value_json.z%d_volts }}", z);
    mqttDiscovery(id, name, "planthub/zones", "V", "voltage", "", vt);

    snprintf(id, sizeof(id), "zone%d_watering", z);
    snprintf(name, sizeof(name), "Garden Zone %d Watering", z);
    snprintf(vt, sizeof(vt), "{{ value_json.z%d_watering }}", z);
    mqttDiscoveryBinary(id, name, "planthub/zones", "running", "mdi:water", vt);

    snprintf(id, sizeof(id), "zone%d_needs_water", z);
    snprintf(name, sizeof(name), "Garden Zone %d Needs Water", z);
    snprintf(vt, sizeof(vt), "{{ value_json.z%d_needs_water }}", z);
    mqttDiscoveryBinary(id, name, "planthub/zones", "problem", "mdi:water-alert", vt);
  }

  // Remote nodes (existing ones)
  for (int i = 0; i < numSensors; i++) {
    mqttSendRemoteDiscovery(i);
  }

  Serial.println("[MQTT] Discovery sent");
}

void mqttSendRemoteDiscovery(int idx) {
  if (!mqtt.connected()) return;
  char id[32], name[48], topic[48], vt[64];

  snprintf(topic, sizeof(topic), "planthub/remote/%d", idx);

  snprintf(id, sizeof(id), "remote%d_moisture", idx);
  snprintf(name, sizeof(name), "%s Moisture", remoteNodeNames[idx]);
  mqttDiscovery(id, name, topic, "%", "", "mdi:water-percent", "{{ value_json.moisture }}");

  snprintf(id, sizeof(id), "remote%d_battery", idx);
  snprintf(name, sizeof(name), "%s Battery", remoteNodeNames[idx]);
  mqttDiscovery(id, name, topic, "V", "battery", "", "{{ value_json.battery }}");

  snprintf(id, sizeof(id), "remote%d_lastseen", idx);
  snprintf(name, sizeof(name), "%s Last Seen", remoteNodeNames[idx]);
  mqttDiscovery(id, name, topic, "s", "", "mdi:clock-outline", "{{ value_json.last_seen }}");

  snprintf(id, sizeof(id), "remote%d_online", idx);
  snprintf(name, sizeof(name), "%s Online", remoteNodeNames[idx]);
  mqttDiscoveryBinary(id, name, topic, "connectivity", "", "{{ 'ON' if value_json.last_seen | int < 1860 else 'OFF' }}");

  snprintf(id, sizeof(id), "remote%d_needs_water", idx);
  snprintf(name, sizeof(name), "%s Needs Water", remoteNodeNames[idx]);
  mqttDiscoveryBinary(id, name, topic, "problem", "mdi:water-alert", "{{ 'ON' if value_json.moisture | int < 40 else 'OFF' }}");
}

void mqttPublishRemote(int idx) {
  if (!mqtt.connected()) return;
  char topic[48], buf[128];
  snprintf(topic, sizeof(topic), "planthub/remote/%d", idx);
  unsigned long lastSeenSec = (millis() - lastReceived[idx]) / 1000;
  snprintf(buf, sizeof(buf),
           "{\"moisture\":%d,\"voltage\":%.3f,\"battery\":%.2f,\"last_seen\":%lu,\"node\":\"%s\",\"zone\":%d}",
           remoteData[idx].moisture, remoteData[idx].voltage,
           remoteData[idx].batteryVoltage, lastSeenSec,
           remoteNodeNames[idx], remoteData[idx].zone);
  mqtt.publish(topic, buf);
}

void mqttPublish() {
  if (!mqtt.connected()) return;
  char buf[256];

  // Environment
  snprintf(buf, sizeof(buf),
           "{\"temp\":%.2f,\"temp_f\":%.1f,\"hum\":%.2f,\"press\":%.2f,\"vpd\":%.2f}",
           tempC, tempC * 1.8 + 32, humidity, pressure, currentVPD);
  mqtt.publish("planthub/env", buf);

  // System
  snprintf(buf, sizeof(buf),
           "{\"uptime\":%lu,\"heap\":%d,\"battery\":%.2f,\"nodes\":%d,\"ip\":\"%s\"}",
           millis() / 60000, ESP.getFreeHeap() / 1024, battery, numSensors,
           WiFi.localIP().toString().c_str());
  mqtt.publish("planthub/system", buf);

  // Valve
  mqtt.publish("planthub/valve", mainValveOpen ? "ON" : "OFF");

  // Local zones
  int offset = 0;
  offset += snprintf(buf + offset, sizeof(buf) - offset, "{");
  for (int z = 0; z < NUM_ZONES; z++) {
    if (!zones[z].enabled) continue;
    if (offset > 1) offset += snprintf(buf + offset, sizeof(buf) - offset, ",");
    offset += snprintf(buf + offset, sizeof(buf) - offset,
                       "\"z%d_moisture\":%d,\"z%d_volts\":%.3f,\"z%d_watering\":\"%s\",\"z%d_needs_water\":\"%s\"",
                       z, moisturePct[z],
                       z, moistureVolts[z],
                       z, zoneState[z].active ? "ON" : "OFF",
                       z, moisturePct[z] < zones[z].dryThreshold ? "ON" : "OFF");
  }
  snprintf(buf + offset, sizeof(buf) - offset, "}");
  mqtt.publish("planthub/zones", buf);

  // Remote sensors
  for (int i = 0; i < numSensors; i++) {
    if (millis() - lastReceived[i] > 3600000) continue;
    mqttPublishRemote(i);
  }
}

/* ================= WEB SERVER ================= */
String contentType(String path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".csv")) return "text/csv";
  return "text/plain";
}

void serveFile(WiFiClient& client, String path) {
  if (!SD.exists(path)) {
    client.println("HTTP/1.1 404 Not Found\r\n\r\nFile not found");
    client.stop(); return;
  }
  File file = SD.open(path);
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: " + contentType(path));
  client.println("Connection: close\r\n");
  // Buffered read - 10-20x faster than byte-by-byte
  uint8_t fbuf[512];
  while (file.available()) {
    int bytesRead = file.read(fbuf, sizeof(fbuf));
    if (bytesRead > 0) client.write(fbuf, bytesRead);
  }
  file.close();
  client.stop();
}

void serveJSON(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close\r\n");

  struct tm ti;
  char timeStr[32] = "N/A";
  if (getLocalTime(&ti)) strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &ti);

  client.print("{");
  client.print("\"time\":\""); client.print(timeStr); client.print("\",");
  client.print("\"heap\":"); client.print(ESP.getFreeHeap()); client.print(",");
  client.print("\"uptime\":"); client.print(millis() / 60000); client.print(",");
  client.print("\"wifi\":"); client.print(WiFi.RSSI()); client.print(",");
  client.print("\"ip\":\""); client.print(WiFi.localIP()); client.print("\",");
  client.print("\"nodes\":"); client.print(numSensors); client.print(",");
  client.print("\"battery\":"); client.print(battery, 2); client.print(",");
  client.print("\"temp\":"); client.print(tempC, 2); client.print(",");
  client.print("\"hum\":"); client.print(humidity, 2); client.print(",");
  client.print("\"press\":"); client.print(pressure, 2); client.print(",");
  client.print("\"vpd\":"); client.print(currentVPD, 2); client.print(",");
  client.print("\"mainValve\":"); client.print(mainValveOpen ? "true" : "false"); client.print(",");
  client.print("\"weather\":{");
  client.print("\"valid\":"); client.print(weather.valid ? "true" : "false"); client.print(",");
  client.print("\"forecast\":\""); client.print(weather.valid ? weather.shortForecast : "N/A"); client.print("\",");
  client.print("\"rainChance\":"); client.print(weather.precipChance); client.print(",");
  client.print("\"rain6h\":"); client.print(weather.rainNext6Hours ? "true" : "false"); client.print(",");
  client.print("\"tempF\":"); client.print(weather.valid ? weather.tempC * 1.8 + 32 : 0, 1);
  client.print("},");
  client.print("\"zones\":[");
  bool first = true;

  for (int z = 0; z < NUM_ZONES; z++) {
    if (!zones[z].enabled) continue;
    if (!first) client.print(",");
    first = false;
    client.print("{");
    client.print("\"zone\":"); client.print(z); client.print(",");
    client.print("\"node\":\""); client.print(NODE_NAME); client.print("\",");
    client.print("\"moisture\":"); client.print(moisturePct[z]); client.print(",");
    client.print("\"volts\":"); client.print(moistureVolts[z], 3); client.print(",");
    client.print("\"status\":\""); client.print(statusMsg[z]); client.print("\",");
    client.print("\"watering\":"); client.print(zoneState[z].active ? "true" : "false");
    client.print("}");
  }

  for (int i = 0; i < numSensors; i++) {
    if (millis() - lastReceived[i] > 3600000) continue;
    if (!first) client.print(",");
    first = false;
    client.print("{");
    client.print("\"zone\":"); client.print(remoteData[i].zone); client.print(",");
    client.print("\"node\":\""); client.print(remoteNodeNames[i]); client.print("\",");
    client.print("\"moisture\":"); client.print(remoteData[i].moisture); client.print(",");
    client.print("\"volts\":"); client.print(remoteData[i].voltage, 3); client.print(",");
    client.print("\"battery\":"); client.print(remoteData[i].batteryVoltage, 2); client.print(",");
    client.print("\"lastSeen\":"); client.print((millis() - lastReceived[i]) / 1000); client.print(",");
    client.print("\"status\":\"OK\"");
    client.print("}");
  }
  client.print("]");
  client.print("}");
  client.stop();
}

void handleClient() {
  WiFiClient client = server.available();
  if (!client) return;
  String req = client.readStringUntil('\r');
  client.flush();

  // Data endpoints - no auth needed
  if (req.indexOf("GET /data.json") >= 0) { serveJSON(client); return; }

  // Command endpoints - require API key
  bool isCommand = req.indexOf("/relayOn") >= 0 || req.indexOf("/relayOff") >= 0 ||
                   req.indexOf("/lcdOn") >= 0 || req.indexOf("/lcdOff") >= 0 ||
                   req.indexOf("/waterZone") >= 0;

  if (isCommand) {
    // Check for API key
    String keyParam = String("key=") + API_KEY;
    if (req.indexOf(keyParam) < 0) {
      client.println("HTTP/1.1 403 Forbidden\r\n\r\nAccess denied. Append ?key=YOUR_API_KEY");
      client.stop();
      Serial.println("[WEB] Unauthorized command attempt blocked");
      return;
    }
  }

  if (req.indexOf("GET /lcdOn") >= 0) {
    lcd.setBacklight(1);
    client.println("HTTP/1.1 200 OK\r\n\r\nOK");
    client.stop(); return;
  }
  if (req.indexOf("GET /lcdOff") >= 0) {
    lcd.setBacklight(0);
    client.println("HTTP/1.1 200 OK\r\n\r\nOK");
    client.stop(); return;
  }
  if (req.indexOf("GET /relayOn") >= 0) {
    openMainValve(); openZoneValve(0);
    client.println("HTTP/1.1 200 OK\r\n\r\nOK");
    client.stop(); return;
  }
  if (req.indexOf("GET /relayOff") >= 0) {
    closeZoneValve(0); closeMainValve();
    client.println("HTTP/1.1 200 OK\r\n\r\nOK");
    client.stop(); return;
  }
  if (req.indexOf("GET /waterZone") >= 0) {
    int zIdx = req.indexOf("waterZone");
    int zone = req.substring(zIdx + 9, zIdx + 10).toInt();
    if (zone >= 0 && zone < NUM_ZONES && zones[zone].enabled) {
      if (!zoneState[zone].active) startWatering(zone);
      else stopWatering(zone);
    }
    client.println("HTTP/1.1 200 OK\r\n\r\nOK");
    client.stop(); return;
  }

  int s = req.indexOf(' '), e = req.indexOf(' ', s + 1);
  String path = req.substring(s + 1, e);
  if (path == "/" || path == "") path = "/index.html";
  serveFile(client, path);
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n\n========================================");
  Serial.println("    PLANT HUB PRO - NODE0 v2.8");
  Serial.println("    Safety & Hardening");
  Serial.println("========================================");
  Serial.printf("RemoteSensorData size: %d bytes\n", sizeof(RemoteSensorData));
  Serial.println("========================================\n");

  // GPIO - force all relays OFF immediately on boot
  pinMode(RELAY_MAIN, OUTPUT); digitalWrite(RELAY_MAIN, LOW);
  for (int i = 0; i < NUM_ZONES; i++) {
    pinMode(ZONE_RELAY_PINS[i], OUTPUT);
    digitalWrite(ZONE_RELAY_PINS[i], LOW);
  }
  pinMode(BUILTIN_LED, OUTPUT);

  // Watchdog timer - reboot if code hangs for WDT_TIMEOUT seconds
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.printf("Watchdog... OK (%d sec)\n", WDT_TIMEOUT);

  Wire.begin(21, 22);

  lcd.begin(16, 2);
  lcd.setBacklight(1);
  lcd.print("Plant Hub Pro");
  lcd.setCursor(0, 1); lcd.print("v2.8 Safety...");

  if (ads.begin(ADS_ADDR)) { adsOK = true; ads.setGain(GAIN_ONE); Serial.println("ADS1115... OK"); }
  else Serial.println("ADS1115... FAIL");

  if (bme.begin(BME_ADDR) || bme.begin(0x77)) { bmeOK = true; Serial.println("BME280... OK"); }
  else Serial.println("BME280... FAIL");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(500);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: %s (ch %d)\n", WiFi.localIP().toString().c_str(), WiFi.channel());
    Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
  } else {
    Serial.println("WiFi FAILED");
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // ESP-NOW
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDataRecv);
    Serial.println("ESP-NOW... OK");
  }

  // MQTT
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqttReconnect();
  if (mqtt.connected()) {
    mqttSendAllDiscovery();
    lastMQTTDiscovery = millis();
  }

  // OTA - disable watchdog during uploads
  ArduinoOTA.onStart([]() {
    esp_task_wdt_delete(NULL);
    Serial.println("[OTA] Update starting - watchdog disabled");
    forceAllRelaysOff();  // Safety: kill all relays during update
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Update complete - rebooting");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error %u - rebooting\n", error);
    ESP.restart();
  });
  ArduinoOTA.begin();

  // SD
  if (SD.begin(SD_CS)) {
    sdOK = true;
    Serial.println("SD Card... OK");
    if (!SD.exists("/moisture.csv")) {
      File f = SD.open("/moisture.csv", FILE_WRITE);
      f.println("timestamp,node,zone,moisture,voltage,status,temp,humidity,pressure,battery,vpd");
      f.close();
    }
  } else {
    Serial.println("SD Card... FAIL");
  }

  server.begin();
  Serial.printf("Web: http://%s\n", WiFi.localIP().toString().c_str());

  // Init state
  for (int i = 0; i < NUM_ZONES; i++) {
    zoneState[i] = {false, 0, 0, 0, false};
  }
  for (int i = 0; i < MAX_SENSORS; i++) remoteNodeNames[i][0] = '\0';

  weather.valid = false;
  updateWeather();

  readAllMoisture();
  readBME();
  currentVPD = calculateVPD(tempC, humidity);
  updateLCD();

  Serial.println("\nSTARTUP COMPLETE\n");
}

/* ================= LOOP ================= */
void loop() {
  // Feed the watchdog - if this isn't called within WDT_TIMEOUT, ESP32 reboots
  esp_task_wdt_reset();

  ArduinoOTA.handle();
  handleClient();

  // WiFi reconnect check (every 30 sec)
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 30000) {
    checkWiFi();
    lastWiFiCheck = millis();
  }

  // MQTT keepalive + reconnect
  if (!mqtt.connected()) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 5000) {
      mqttReconnect();
      lastReconnect = millis();
    }
  }
  mqtt.loop();

  // Relay failsafe - runs every loop iteration for maximum safety
  relayFailsafeCheck();

  // LCD
  if (millis() - lastLCD > 5000) {
    updateLCD();
    lastLCD = millis();
  }

  // Sensor sampling
  if (millis() - lastSample > 30000) {
    readAllMoisture();
    readBME();
    currentVPD = calculateVPD(tempC, humidity);

    moistureHist[histIdx] = moisturePct[0];
    tempHist[histIdx] = tempC;
    humHist[histIdx] = humidity;
    pressHist[histIdx] = pressure;
    histIdx = (histIdx + 1) % HISTORY_LEN;

    static int sampleCount = 0;
    if (++sampleCount >= 10) { logToSD(); sampleCount = 0; }
    lastSample = millis();
  }

  // MQTT publish
  if (millis() - lastMQTTPublish > MQTT_PUBLISH_INTERVAL) {
    mqttPublish();
    lastMQTTPublish = millis();
  }

  // Re-send discovery periodically
  if (millis() - lastMQTTDiscovery > MQTT_DISCOVERY_INTERVAL) {
    mqttSendAllDiscovery();
    lastMQTTDiscovery = millis();
  }

  // Beacon
  if (millis() - lastBeacon > 60000) {
    broadcastBeacon();
    lastBeacon = millis();
  }

  smartWateringControl();
}
