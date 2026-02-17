# Plant Hub Pro

Smart irrigation system with ESP-NOW mesh networking, MQTT, and Home Assistant integration.

## What It Does

Plant Hub Pro is a multi-zone garden irrigation controller that takes watering decisions based on real-time soil moisture, temperature, humidity, pressure, and VPD (vapor pressure deficit). It uses NOAA weather forecasts to skip watering during predicted rain and includes safety features like watchdog timers, relay failsafes, and API key protection.

## Features

- 4 controllable zones + remote battery-powered sensors via ESP-NOW
- VPD-based dynamic thresholds + hysteresis (40% on, 70% off) + cooldown periods
- NOAA weather forecast integration (precip chance, temp)
- Custom web dashboard with live charts and zone controls
- SD card CSV logging
- MQTT + Home Assistant auto-discovery
- Hardware watchdog, relay failsafe, API key protection
- 16×2 LCD status display

## Screenshots

NA  Yet
## Hardware

- ESP32 Dev Module
- ADS1115 16-bit ADC
- BME280 (temp/hum/pressure)
- Capacitive soil moisture sensors
- 8-channel relay module
- 16×2 LCD with I2C
- Micro SD card module
- LiPo + TP4056 for remote nodes


## License

MIT License


Proprietary - 518 IT Solutions LLC

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| v2.8 | 2026-02-13 | Watchdog, relay failsafe, API auth, buffered web server, weather card |
| v2.7 | 2026-02-12 | MQTT + HA auto-discovery |
| v2.6 | 2026-02-11 | ESP-NOW struct fix, remote sensor support |
