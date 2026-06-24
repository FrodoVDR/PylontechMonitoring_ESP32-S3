# Pylontech Battery Monitoring

> **Disclaimer: I take no responsibility for any damage. Use at your own risk.**

ESP32-S3-based firmware for monitoring Pylontech batteries (US2000B, US2000C, US3000C, US5000) via Wi-Fi or Ethernet.
Forked from [irekzielinski/Pylontech-Battery-Monitoring](https://github.com/irekzielinski/Pylontech-Battery-Monitoring), [@hidaba](https://github.com/hidaba) and significantly reworked for ESP32-S3 by [@HeldvomForst](https://github.com/HeldvomForst/PylontechMonitoring_ESP32).

---

## Table of Contents

1. [Features](#features)
2. [Hardware](#hardware)
3. [Wiring](#wiring)
4. [Initial Installation](#initial-installation)
5. [Web Interface](#web-interface)
6. [MQTT](#mqtt)
7. [OTA Update](#ota-update)
8. [Boot Button Functions](#boot-button-functions)
9. [Configuration Parameters](#configuration-parameters)
10. [Architecture](#architecture)
11. [Changelog](#changelog)

---

## Features

- **Dual-core architecture** - UART communication on core 0, web/MQTT/Wi-Fi on core 1
- **Web interface** - dashboard, cell data, statistics, health view, console, log
- **MQTT support** - publishes battery status to any broker; Home Assistant autodiscovery
- **OTA firmware updates** - directly from the web interface (no USB required)
- **Ethernet support** - W5500 SPI (for example Waveshare ESP32-S3-ETH)
- **DHCP or static IP** - independently configurable for Wi-Fi and Ethernet
- **Display support** - ST7735 TFT (optional)
- **NTP time sync** - configurable timezone and DST
- **Health monitoring** - cell voltage delta warning/error thresholds
- **Settings backup/restore** - JSON export/import via web interface
- **Factory reset** - via button or web interface
- **Robust INFO/STAT/BAT settings** - NVS cache and last-good fallback to avoid empty pages after navigation
- **Stable scheduler queue** - mutex, deduplication, and queue limit to prevent refresh storms

---

## Hardware

| Component | Description |
|---|---|
| ESP32-S3 WROOM | Microcontroller (4 MB Flash, 8 MB PSRAM) |
| MAX3232 transceiver | RS-232 to UART level shifter |
| Capacitor C1 | 10 uF (recommended, power stabilization) |
| Capacitor C2 | 0.1 uF (recommended) |
| Cable US2000B | RJ11 (4 wires) |
| Cable US2000C / US3000C / US5000 | RJ45 |

---

## Wiring

### General (MAX3232 to ESP32-S3)

```text
MAX3232 T1IN  -> ESP32 TX  (GPIO 17)
MAX3232 R1OUT -> ESP32 RX  (GPIO 16)
MAX3232 GND   -> GND
MAX3232 VCC   -> 3.3 V
```

### Pylontech US2000B - RJ11

```text
RJ11 Pin 2 -> MAX3232 R1IN
RJ11 Pin 3 -> MAX3232 T1OUT
RJ11 Pin 4 -> GND
```

### Pylontech US2000C / US3000C / US5000 - RJ45

```text
RJ45 Pin 3 (white/green) -> MAX3232 R1IN
RJ45 Pin 6 (green)       -> MAX3232 T1OUT
RJ45 Pin 8 (brown)       -> GND
```

Connect to the **master battery** if multiple batteries are present.

![Schematic](Schemetics.png)

---

## Initial Installation

### 1. Compile and flash firmware (first time via USB)

Requirement: [arduino-cli](https://arduino.github.io/arduino-cli/) and ESP32 core installed.

```bash
# Provide partition table
cp partitions/pylontech_ota_spiffs.csv partitions.csv

# Compile
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=4M" \
  --libraries libraries/ \
  --output-dir build/ .

# Upload
arduino-cli upload \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=4M" \
  --port /dev/tty.usbmodem<XXXX> \
  --input-dir build/ .
```

### 2. Upload web files

1. Device starts as Wi-Fi hotspot `pylontech-XXXX`
2. Open `http://192.168.4.1/filemanager`
3. Upload all files from folder `data/`
4. Open `http://192.168.4.1`

### 3. Configure Wi-Fi

1. Open page `Connection` -> **Scan** -> select SSID -> enter password -> save
2. Wait about 30 seconds (AP turns off automatically)
3. New IP is shown on dashboard after reconnect

All pages should be reachable within 60 seconds after boot.

---

## Web Interface

| URL | Description |
|---|---|
| `/` | Dashboard (SOC, voltage, current, temperature) |
| `/celldata` | Cell voltages of all modules |
| `/health` | Health status (cell voltage deltas) |
| `/statistic` | Statistic data |
| `/info` | INFO data (device metadata, barcode, software version) |
| `/log` | System log |
| `/console` | Direct battery console |
| `/connect` | Wi-Fi, MQTT, NTP, IP settings |
| `/service` | OTA update, restart, backup/restore, factory reset |
| `/filemanager` | SPIFFS file manager |

---

## MQTT

### Connection settings

In web interface under `Connection -> MQTT Settings`:

| Parameter | Default |
|---|---|
| Server | `192.168.8.4` |
| Port | `1883` |
| Prefix | `Pylontech` |
| Stack topic | `Stack` |

### Topics

```text
Pylontech/Stack/<field>         - Stack summary values (SOC, current, voltage ...)
Pylontech/pwr/<module>/<field>  - Per-module values (PWR command)
Pylontech/bat/<module>/Cell<N>  - Cell values (BAT command)
Pylontech/stat/<module>/<field> - Statistics (STAT command)
Pylontech/info/<module>/<field> - INFO fields (for example barcode, software version)
```

### Home Assistant autodiscovery

- **MQTT** checkbox = value is published
- **Send** checkbox = autodiscovery payload is published

---

## OTA Update

Works only if partition table was flashed correctly during first USB installation.

1. Open `http://<ESP-IP>/service`
2. Select `build/PylontechMonitoring.ino.bin`
3. Click **Flash**
4. Device reboots automatically

File name requirement: `.bin` file must contain `Pylontech` in its name.

---

## Boot Button Functions

| Button action | Effect |
|---|---|
| 1x short press | Enable Wi-Fi access point |
| 5x short press | Reset Wi-Fi settings |
| Hold 15 s | Factory reset (clear all settings) |

---

## Configuration Parameters

All settings are stored in ESP32 NVS and survive firmware updates.

### System

| Parameter | Default | Description |
|---|---|---|
| `deviceName` | `PylontechMonitor` | Display name |
| `hostname` | `pylontech-XXXX` | mDNS host name (derived from MAC) |
| `firmwareVersion` | `1.2.6` | Current firmware version |

### Wi-Fi / Network

| Parameter | Description |
|---|---|
| SSID / password | Wi-Fi credentials |
| Static IP | Optional IP, subnet, gateway, DNS |
| Ethernet (W5500) | MISO=12, MOSI=11, SCK=13, CS=14, RST=9, INT=10 |

### MQTT

| Parameter | Default | Description |
|---|---|---|
| `mqtt.enabled` | `true` | Enable MQTT |
| `mqtt.server` | `192.168.8.4` | Broker address |
| `mqtt.port` | `1883` | Broker port |
| `mqtt.prefix` | `Pylontech` | Topic prefix |
| `mqtt.mode` | `active` | Publish mode |

### Battery polling intervals

| Parameter | Default | Description |
|---|---|---|
| `intervalPwr` | 60000 ms | PWR polling (voltage, current, SOC) |
| `intervalBat` | 300000 ms | BAT polling (cell values) |
| `intervalStat` | 1800000 ms | STAT polling (statistics) |
| `intervalInfo` | 3600000 ms | INFO polling (barcode/version/device metadata) |

### Health thresholds

| Parameter | Default | Description |
|---|---|---|
| `cellDiffWarn` | 10 mV | Cell voltage delta warning |
| `cellDiffError` | 20 mV | Cell voltage delta error |

---

## Architecture

```text
ESP32-S3 (Dual Core)
|
|-- Core 0 - Realtime Task
|   `-- UART <-> Pylontech RS-232
|       `-- Frame parser (PWR / BAT / STAT / INFO)
|           `-- Double buffer (pwrA/B, batA/B, statA/B, infoA/B)
|
`-- Core 1 - Non-critical Task
    |-- Scheduler (command queue with mutex + deduplication)
    |-- MQTT (PubSubClient)
    |-- Webserver (WebServer)
    |-- API cache (NVS + last-good fallback)
    |-- WiFiManager / EthManager
    |-- SystemManager
    `-- Display (ST7735, every 500 ms)
```

### Partition table (4 MB flash)

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB |
| otadata | data/ota | 0xE000 | 8 KB |
| app0 (OTA 0) | app | 0x10000 | 1792 KB |
| app1 (OTA 1) | app | 0x1D0000 | 1792 KB |
| spiffs | data/spiffs | 0x390000 | 448 KB |

---

## Changelog

See [CHANGELOG.md](CHANGELOG.md)
