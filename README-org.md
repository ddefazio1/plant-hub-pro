# Plant Hub Pro

**Smart irrigation system with ESP-NOW mesh networking, MQTT, and Home Assistant integration.**

Built by 518 IT Solutions LLC | Troy, NY

---

## What It Does

Plant Hub Pro is a multi-zone garden irrigation controller that makes watering decisions based on real-time soil moisture, VPD (Vapor Pressure Deficit), and NOAA weather forecasts. It skips watering when rain is coming and waters more aggressively when conditions are hot and dry.

### Features

- **Multi-zone soil moisture monitoring** via ADS1115 16-bit ADC
- **Wireless remote sensors** using ESP-NOW (low power, battery-friendly)
- **VPD-aware watering** - adjusts thresholds based on temperature and humidity
- **NOAA weather integration** - skips watering when rain is forecast
- **MQTT with Home Assistant auto-discovery** - all sensors appear automatically
- **Ocean Glass web dashboard** - real-time monitoring from any browser
- **Automatic relay control** - main valve + per-zone solenoid valves
- **Safety systems** - hardware watchdog, relay failsafe, emergency shutoff
- **SD card logging** - historical moisture, temperature, and watering data
- **OTA updates** - flash firmware over WiFi

---

## System Architecture

```
                         WiFi + MQTT
                    +-------------------> Home Assistant
                    |                     (auto-discovery)
                    |
  NODE0 (Hub)  -----+---> Web Dashboard (served from SD card)
   ESP32            |
   ADS1115          |     ESP-NOW (wireless)
   BME280      <----+------------------------  NODE1 (Remote)
   LCD 16x2                                     ESP32
   SD Card                                      Soil Sensor
   Relay Board                                  Battery + Solar
   Soil Sensors                                 Deep Sleep 30 min
                                           
                   ESP-NOW (wireless)
              <----+------------------------  NODE2, NODE3... (auto-register)
```

### How Watering Decisions Are Made

1. Soil moisture drops below dry threshold (40%)
2. VPD check - hot/dry air raises threshold, cool/humid lowers it
3. NOAA weather check - skip if rain forecast >60% in next 6 hours
4. Temperature check - water sooner if >86F (30C)
5. Cooldown check - wait 60 min between waterings per zone
6. Hysteresis - don't stop until moisture hits 70% (wet threshold)
7. Emergency mode - moisture below 20% overrides all checks

---

## Hardware

### NODE0 (Hub)

| Component | Purpose |
|-----------|---------|
| ESP32 Dev Module | Main controller |
| ADS1115 | 16-bit ADC for soil sensors |
| BME280 | Temperature, humidity, pressure |
| LCD 16x2 (I2C) | Local status display |
| SD Card Module | Web files + data logging |
| 8-Channel Relay Board | Valve control |
| Capacitive Soil Sensors | Moisture sensing (up to 4 local zones) |

### NODE1+ (Remote Sensors)

| Component | Purpose |
|-----------|---------|
| ESP32 Dev Module | Sensor + wireless transmitter |
| Capacitive Soil Sensor | Moisture sensing |
| TP4056 USB-C | LiPo charging (solar compatible) |
| 3.7V LiPo Battery | 1000mAh+ recommended |
| 2x 10K Resistors | Battery voltage divider on GPIO 35 |

### Pin Map (NODE0)

| ESP32 Pin | Connects To |
|-----------|-------------|
| 3.3V | LCD, ADS1115, BME280, Soil Sensors |
| 5V (VIN) | Relay Board VCC |
| GND | Everything (common ground) |
| GPIO 21 | SDA - LCD, ADS1115, BME280 |
| GPIO 22 | SCL - LCD, ADS1115, BME280 |
| GPIO 13 | SD Card CS |
| GPIO 23 | SD Card MOSI |
| GPIO 19 | SD Card MISO |
| GPIO 18 | SD Card CLK |
| GPIO 25 | Relay IN1 (Main Valve) |
| GPIO 26 | Relay IN2 (Zone 0) |
| GPIO 27 | Relay IN3 (Zone 1) |
| GPIO 14 | Relay IN4 (Zone 2) |
| GPIO 12 | Relay IN5 (Zone 3) |

---

## Software

### File Structure

```
plant-hub-pro/
  firmware/
    node0/NODE0_v28.ino       # Hub firmware
    node1/NODE1_v12.ino       # Remote sensor firmware
  web/
    index.html                 # Ocean Glass dashboard
    style.css                  # Teal gradient, frosted glass
    script.js                  # Auto-populating zones, charts
  homeassistant/
    plant_hub_templates.yaml   # HA template sensors (legacy)
    lovelace_dashboard.yaml    # HA dashboard layout
  docs/
    wiring_diagram.svg         # Complete wiring reference
```

### Dependencies

**Arduino Libraries (install via Library Manager):**
- PubSubClient (Nick O'Leary) - MQTT
- Adafruit ADS1X15 - ADC
- Adafruit BME280 - Environmental sensor
- LiquidCrystal PCF8574 - LCD
- ArduinoJson - JSON parsing
- SD, SPI, Wire - Built-in

**ESP32 Board:** Arduino ESP32 core 3.x

**Home Assistant:**
- Mosquitto MQTT broker add-on
- MQTT integration

---

## Setup

### 1. Flash NODE0

1. Open `firmware/node0/NODE0_v28.ino` in Arduino IDE
2. Edit the config section:
   - `WIFI_SSID` and `WIFI_PASS` - your WiFi credentials
   - `MQTT_SERVER` - your Home Assistant IP
   - `MQTT_USER` and `MQTT_PASS` - MQTT credentials
   - `API_KEY` - web dashboard command authentication key
3. Board: ESP32 Dev Module
4. Upload via USB

### 2. Set Up SD Card

Copy these files to the root of a FAT32 SD card:
- `web/index.html`
- `web/style.css`
- `web/script.js`

Insert the SD card into NODE0's SD card module.

### 3. Flash NODE1 (Remote Sensors)

1. Open `firmware/node1/NODE1_v12.ino`
2. Set `ZONE_ID` to a unique number (1, 2, 3...)
3. Set `HUB_MAC` to NODE0's MAC address (shown on NODE0 serial monitor at boot)
4. Upload via USB
5. Power with USB or LiPo battery

NODE1 auto-registers with NODE0 - no hub changes needed.

### 4. Home Assistant

1. Install Mosquitto broker add-on
2. Create a dedicated MQTT user (Settings > People > Users)
3. Add MQTT integration (Settings > Devices & Services)
4. Plant Hub device appears automatically via MQTT auto-discovery

---

## Web Dashboard

Access at `http://<NODE0_IP>` from any device on your network.

- Real-time moisture, temperature, humidity, pressure, VPD
- NOAA weather forecast with rain prediction
- Remote sensor cards auto-populate when nodes check in
- Manual watering controls (authenticated with API key)
- Historical charts (moisture, temp, humidity, pressure)
- CSV data download
- F/C and inHg/hPa toggles
- Responsive design (desktop, tablet, mobile)

---

## Safety Features

| Feature | Description |
|---------|-------------|
| Hardware Watchdog | 30-second timeout, auto-reboots if code hangs |
| Relay Failsafe | All relays forced OFF on boot |
| Hard Timeout | 15 min max for any relay, then forced off |
| Emergency Shutoff | All relays killed during OTA updates |
| API Authentication | Web commands require API key |
| WiFi Auto-Reconnect | Recovers from WiFi drops every 30 sec |
| MQTT Auto-Reconnect | Recovers from broker disconnects every 5 sec |
| MQTT LWT | Home Assistant knows if hub goes offline |
| Relay Alerts | MQTT alert sent if hard timeout triggers |

---

## Adding New Remote Nodes

1. Copy `NODE1_v12.ino`
2. Change `ZONE_ID` to a unique number
3. Enter NODE0's MAC address
4. Flash and power on
5. Node auto-registers everywhere (web dashboard, MQTT, HA)

NODE0 supports up to 10 remote sensors.

---

## Configuration

Key settings in `NODE0_v28.ino`:

```c
// Moisture thresholds (per zone)
zones[0] = {2.691, 0.971, true, 40, 70, 120, 60};
//           dry_v  wet_v  on   dry% wet% secs cooldown_min

// Watering behavior
#define MAX_WATERING_TIME       600     // 10 min safety max (code level)
#define HARD_RELAY_TIMEOUT      900     // 15 min absolute max (hardware level)

// Timing
#define MQTT_PUBLISH_INTERVAL   15000   // Publish to HA every 15 sec
#define WEATHER_UPDATE_INTERVAL 3600000 // NOAA check every hour
#define WDT_TIMEOUT             30      // Watchdog timeout seconds
```

---

## License

Proprietary - 518 IT Solutions LLC

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v2.8 | 2026-02-13 | Watchdog, relay failsafe, API auth, buffered web server, weather card |
| v2.7 | 2026-02-12 | MQTT + HA auto-discovery |
| v2.6 | 2026-02-11 | ESP-NOW struct fix, remote sensor support |
