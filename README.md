# ESP32-P4 Smart Display

ESP-IDF v5.4 firmware for a 10.1" smart home control panel based on the ESP32-P4 + ESP32-C6 module.

## Hardware

| | |
|---|---|
| Board | JC8012P4A1C_I_W_Y (Guition 10", 800×1280) |
| MCU | ESP32-P4 (host) + ESP32-C6 (WiFi via SDIO) |
| Display | JD9365 DSI, IPS, capacitive touch |
| PSRAM | 32 MB, 200 MHz HEX mode |
| Flash | 16 MB |

## Features

- LVGL v9 dashboard UI
- Home Assistant integration via MQTT
- WiFi setup via captive portal (AP mode fallback)
- OTA firmware update via HTTP POST `/ota`
- Web configuration portal

## Building

Requires Podman and the `localhost/concept_idf:latest` container image.

```bash
# Build
./firmware/build.sh

# Flash via USB
./firmware/flash.sh

# OTA update (device must be on network)
curl -X POST http://<device-ip>/ota --data-binary @firmware/build/concept.bin
```

## Configuration

On first boot the device starts in AP mode (`SmartDisplay-XXXX`).
Connect to it and open `http://192.168.4.1/` to configure:

- WiFi credentials
- MQTT broker URL (format: `mqtt://user:pass@192.168.1.x:1883`)
- Home Assistant entity IDs

WiFi credentials are stored in NVS. MQTT and entity config is stored in SPIFFS at `/spiffs/config.json`.

## Project structure

```
firmware/
├── main/
│   ├── config/       # SPIFFS JSON config + NVS WiFi credentials
│   ├── net/          # WiFi manager (STA + AP fallback)
│   ├── ha/           # Home Assistant MQTT client
│   ├── ui/           # LVGL dashboard
│   └── server/       # Web config portal + OTA endpoint
├── sdkconfig.defaults
├── partitions.csv
└── build.sh / flash.sh
```
