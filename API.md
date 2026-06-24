# API Dokumentation (PylontechMonitoring)

Stand: automatisch aus dem aktiven Routing abgeleitet.
Quelle der aktiven Registrierung: wp_routes.cpp (inkl. registerCombinedAPI(), registerServiceAPI() und spezialisierte API-Header).

## Basis

- Protokoll: HTTP
- Port: 80
- Authentifizierung: keine (LAN)
- Antwortformate: application/json oder text/plain

## HTTP Statuscodes

- 200: Erfolg
- 400: Ungueltige Anfrage (z. B. Missing body, Invalid JSON)
- 404: Ressource/Datei nicht gefunden
- 413: Payload zu gross
- 500: Interner Fehler (z. B. OTA/Datei-Operation)

## Wichtige Hinweise

- Viele GET-Endpunkte liefern gecachte Werte aus NVS und triggern bei Bedarf eine neue UART-Abfrage.
- Einige POST-Endpunkte erwarten JSON in plain (Body).
- OTA-Update erfolgt per Multipart-Upload auf /api/ota.

## Endpoint Uebersicht

## Dashboard und Runtime

### GET /api/dashboard
- Zweck: Aggregierte Live-Ansicht (WiFi, MQTT, Batterie, Systemzeit, LAN)
- Antwort: JSON

### GET /api/log
- Zweck: Web-Logbuffer lesen
- Antwort: text/plain

### GET /api/log/level
- Zweck: Aktuelle Loglevel und Syslog-Konfiguration
- Antwort: JSON

### POST /api/log/level
- Zweck: Loglevel/Syslog setzen
- Body: JSON (optionale Felder)
- Antwort: text/plain (Log level updated)

## Konsole und Frames

### GET /req?code=<kommando>
- Zweck: UART-Kommando in Scheduler-Warteschlange stellen
- Antwort: text/plain (OK)

### GET /api/lastframe
- Zweck: Wartet bis zu 2 Sekunden auf naechsten Frame
- Antwort: text/plain (Frame oder TIMEOUT)

### GET /api/framedump
- Zweck: Letzten rohen UART-Frame lesen
- Antwort: text/plain

## PWR API

### GET /api/pwr/base
- Zweck: PWR-Basisdaten + Feldkonfiguration
- Antwort: JSON mit config, mqtt, headers, values, fields, cacheTimestamp

### POST /api/pwr/set
- Zweck: PWR-Konfiguration speichern
- Body: JSON
- Antwort: text/plain (PWR settings saved)

### POST /api/pwr/refresh
- Zweck: Cache loeschen und PWR-Abfrage triggern
- Antwort: text/plain (PWR refresh queued)

## BAT API

### GET /api/bat/cells
- Zweck: BAT-Zelldaten + Feldkonfiguration
- Antwort: JSON mit config, mqtt, headers, values, fields, cacheTimestamp, statusText, dataSource

### POST /api/bat/set
- Zweck: BAT-Konfiguration speichern
- Antwort: text/plain (BAT settings saved)

### POST /api/bat/refresh
- Zweck: BAT-Refresh triggern (aktuell Modul 1)
- Antwort: text/plain

## STAT API

### GET /api/stat/values
- Zweck: STAT-Werte + Feldkonfiguration
- Antwort: JSON mit config, mqtt, headers, values, fields, cacheTimestamp, statusText, dataSource

### POST /api/stat/set
- Zweck: STAT-Konfiguration speichern
- Antwort: text/plain (STAT settings saved)

### POST /api/stat/refresh
- Zweck: STAT-Refresh triggern (exklusiver Lauf)
- Antwort: text/plain

## INFO API

### GET /api/info/values
- Zweck: INFO-Werte + Feldkonfiguration (inkl. Last-Good-Fallback)
- Antwort: JSON mit config, mqtt, headers, values, fields, cacheTimestamp, statusText, dataSource

### POST /api/info/set
- Zweck: INFO-Konfiguration speichern
- Antwort: text/plain (INFO settings saved)

### POST /api/info/refresh
- Zweck: INFO-Refresh triggern (alle Module, exklusiv)
- Antwort: text/plain

## Health API

### GET /api/health
- Zweck: Health-Zustand aller Module
- Antwort: JSON mit modules, stack, ok, warn, error, warnHistory, errorHistory, strongest, color, config

### POST /api/health
- Zweck: Health-Schwellen speichern
- Body: JSON
- Antwort: text/plain (Health config saved)

### GET /api/health/reset
- Zweck: Health-Historie zuruecksetzen
- Antwort: text/plain (OK)

## Connectivity API

### GET /api/wifi
### POST /api/wifi
### GET /api/wifi/scan
### GET /api/eth
### POST /api/eth
### GET /api/mqtt
### POST /api/mqtt
### GET /api/time
### POST /api/time
### GET /api/network
### POST /api/network

- Zweck: Lesen/Speichern von WiFi, Ethernet, MQTT, Zeit/NTP und IP-Konfiguration.

## System und Service API

### GET /service
### POST /api/restart
### POST /api/factoryreset
### POST /api/wifireset
### POST /api/ota
### GET /api/version
### GET /api/storageinfo
### GET /api/logfile/status
### POST /api/logfile/enable?enable=1|0
### POST /api/logfile/clear
### GET /api/backup
### POST /api/restore

- Zweck: Service-Seite, Wartung, OTA, Version, Storage, Logfile, Backup/Restore.

## CPU API

### GET /api/cpu
- Zweck: FreeRTOS-Taskliste und Heap-Werte
- Antwort: JSON mit tasks, heap_free, heap_min, heap_psram, heap_psram_total

## Monitoring API

### GET /api/monitoring
- Zweck: Kompakter Monitoring-Endpoint fuer Uptime, CPU-Load pro Core, Memory, SPIFFS und NVS
- Antwort: JSON mit uptime, cpu, memory, spiffs, nvs

## Filemanager API

### GET /filemanager
### GET /fm/list
### GET /fm/download?file=/name
### GET /fm/delete?file=/name
### POST /fm/upload

- Zweck: Dateioperationen auf SPIFFS.

## Endpoint-Tabelle (kompakt)

| Methode | Pfad | Request-Body | Erfolg | Fehlercodes |
| --- | --- | --- | --- | --- |
| GET | /api/dashboard | - | 200 JSON (Dashboarddaten) | - |
| GET | /api/log | - | 200 text/plain | - |
| GET | /api/log/level | - | 200 JSON | - |
| POST | /api/log/level | JSON optional (info,warn,error,debug,syslogEnabled,syslogServer,syslogPort) | 200 text/plain | 400 |
| GET | /req?code=<kommando> | Query: code | 200 text/plain (OK) | - |
| GET | /api/lastframe | - | 200 text/plain (Frame oder TIMEOUT) | - |
| GET | /api/framedump | - | 200 text/plain | - |
| GET | /api/pwr/base | - | 200 JSON | - |
| POST | /api/pwr/set | JSON (config,mqtt,fields) | 200 text/plain | 400 |
| POST | /api/pwr/refresh | - | 200 text/plain | - |
| GET | /api/bat/cells | - | 200 JSON | - |
| POST | /api/bat/set | JSON (config,mqtt,fields) | 200 text/plain | 400 |
| POST | /api/bat/refresh | - | 200 text/plain | - |
| GET | /api/stat/values | - | 200 JSON | - |
| POST | /api/stat/set | JSON (config,mqtt,fields) | 200 text/plain | 400,413 |
| POST | /api/stat/refresh | - | 200 text/plain | - |
| GET | /api/info/values | - | 200 JSON | - |
| POST | /api/info/set | JSON (config,mqtt,fields) | 200 text/plain | 400,413 |
| POST | /api/info/refresh | - | 200 text/plain | - |
| GET | /api/health | - | 200 JSON | - |
| POST | /api/health | JSON (cellDiffWarn,cellDiffError) | 200 text/plain | 400 |
| GET | /api/health/reset | - | 200 text/plain (OK) | - |
| GET | /api/wifi | - | 200 JSON | - |
| POST | /api/wifi | JSON (ssid,pass) | 200 text/plain | 400 |
| GET | /api/wifi/scan | - | 200 JSON | - |
| GET | /api/eth | - | 200 JSON | - |
| POST | /api/eth | JSON (enabled,pins,eth_dhcp,eth_ip,...) | 200 text/plain | 400 |
| GET | /api/mqtt | - | 200 JSON | - |
| POST | /api/mqtt | JSON (enabled,server,port,user,pass,topic) | 200 text/plain | 400 |
| GET | /api/time | - | 200 JSON | - |
| POST | /api/time | JSON (manual_mode,manual_date,manual_time,server,timezone,...) | 200 text/plain | 400 |
| GET | /api/network | - | 200 JSON | - |
| POST | /api/network | JSON (dhcp,ip,mask,gw,dns) | 200 text/plain | 400 |
| GET | /api/cpu | - | 200 JSON | - |
| GET | /api/monitoring | - | 200 JSON (uptime,cpu,memory,spiffs,nvs) | - |
| GET | /service | - | 200 text/html | - |
| POST | /api/restart | - | 200 text/plain | - |
| POST | /api/factoryreset | - | 200 text/plain | - |
| POST | /api/wifireset | - | 200 text/plain | - |
| POST | /api/ota | multipart/form-data (.bin) | 200 text/plain | 400,500 |
| GET | /api/version | - | 200 text/plain | - |
| GET | /api/storageinfo | - | 200 JSON | - |
| GET | /api/logfile/status | - | 200 JSON | - |
| POST | /api/logfile/enable?enable=1|0 | Query: enable | 200 text/plain | - |
| POST | /api/logfile/clear | - | 200 text/plain | 500 |
| GET | /api/backup | - | 200 application/json (Download) | - |
| POST | /api/restore | JSON (Backup-Struktur) | 200 text/plain | 400 |
| GET | /filemanager | - | 200 text/html | - |
| GET | /fm/list | - | 200 JSON | - |
| GET | /fm/download?file=/name | Query: file | 200 application/octet-stream | 400,404 |
| GET | /fm/delete?file=/name | Query: file | 200 text/plain (OK) | 400,404 |
| POST | /fm/upload | multipart/form-data | 200 text/plain (OK) | - |

## Minimale JSON-Schemata (Auszug)

### POST /api/log/level Request

{
  "info": "boolean, optional",
  "warn": "boolean, optional",
  "error": "boolean, optional",
  "debug": "boolean, optional",
  "syslogEnabled": "boolean, optional",
  "syslogServer": "string, optional",
  "syslogPort": "number, optional"
}

### GET /api/log/level Response

{
  "info": "boolean",
  "warn": "boolean",
  "error": "boolean",
  "debug": "boolean",
  "syslogEnabled": "boolean",
  "syslogServer": "string",
  "syslogPort": "number"
}

### POST /api/pwr/set Request

{
  "config": {
    "intervalPwr": "number(ms)",
    "useFahrenheit": "boolean"
  },
  "mqtt": {
    "topicStack": "string",
    "topicPwr": "string"
  },
  "fields": [
    {
      "name": "string",
      "display": "string",
      "factor": "string",
      "unit": "string",
      "sendMQTT": "boolean",
      "sendPayload": "boolean"
    }
  ]
}

### GET /api/pwr/base Response

{
  "config": "object",
  "mqtt": "object",
  "headers": "string[]",
  "values": "string[]",
  "fields": "object[]",
  "cacheTimestamp": "number"
}

### GET /api/bat/cells, /api/stat/values, /api/info/values Response

{
  "config": "object",
  "mqtt": "object",
  "headers": "string[]",
  "values": "string[]",
  "fields": "object[]",
  "cacheTimestamp": "number",
  "statusText": "string",
  "dataSource": "cache|uart|none"
}

### POST /api/health Request

{
  "cellDiffWarn": "number",
  "cellDiffError": "number"
}

### GET /api/health Response

{
  "modules": "object[]",
  "stack": "object",
  "ok": "number[]",
  "warn": "number[]",
  "error": "number[]",
  "warnHistory": "number[]",
  "errorHistory": "number[]",
  "strongest": "string",
  "color": "string",
  "config": "object"
}

### GET /api/logfile/status Response

{
  "enabled": "boolean",
  "path": "string"
}

## Schnelltests mit curl

curl -s http://<esp-ip>/api/version
curl -s http://<esp-ip>/api/dashboard
curl -s http://<esp-ip>/api/pwr/base
curl -s -X POST http://<esp-ip>/api/pwr/refresh
curl -s -X POST "http://<esp-ip>/api/logfile/enable?enable=1"
