# API Documentation (PylontechMonitoring)

Status: generated from active route registration.
Active registration source: wp_routes.cpp (including registerCombinedAPI(), registerServiceAPI(), and specialized API headers).

## Base

- Protocol: HTTP
- Port: 80
- Authentication: none (LAN)
- Response types: application/json or text/plain

## HTTP Status Codes

- 200: success
- 400: bad request (for example Missing body, Invalid JSON)
- 404: resource/file not found
- 413: payload too large
- 500: internal error (for example OTA/file operation)

## Important Notes

- Many GET endpoints return NVS-cached data and may trigger UART refresh.
- Some POST endpoints expect JSON in plain body.
- OTA update is multipart upload to /api/ota.

## Endpoint Overview

## Dashboard and Runtime

- GET /api/dashboard
- GET /api/log
- GET /api/log/level
- POST /api/log/level

## Console and Frames

- GET /req?code=<command>
- GET /api/lastframe
- GET /api/framedump

## PWR API

- GET /api/pwr/base
- POST /api/pwr/set
- POST /api/pwr/refresh

## BAT API

- GET /api/bat/cells
- POST /api/bat/set
- POST /api/bat/refresh

## STAT API

- GET /api/stat/values
- POST /api/stat/set
- POST /api/stat/refresh

## INFO API

- GET /api/info/values
- POST /api/info/set
- POST /api/info/refresh

## Health API

- GET /api/health
- POST /api/health
- GET /api/health/reset

## Connectivity API

- GET /api/wifi
- POST /api/wifi
- GET /api/wifi/scan
- GET /api/eth
- POST /api/eth
- GET /api/mqtt
- POST /api/mqtt
- GET /api/time
- POST /api/time
- GET /api/network
- POST /api/network

## System and Service API

- GET /service
- POST /api/restart
- POST /api/factoryreset
- POST /api/wifireset
- POST /api/ota
- GET /api/version
- GET /api/storageinfo
- GET /api/logfile/status
- POST /api/logfile/enable?enable=1|0
- POST /api/logfile/clear
- GET /api/backup
- POST /api/restore

## CPU API

- GET /api/cpu

## Filemanager API

- GET /filemanager
- GET /fm/list
- GET /fm/download?file=/name
- GET /fm/delete?file=/name
- POST /fm/upload

## Minimal JSON Schemas (Selected)

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

## Quick curl checks

curl -s http://<esp-ip>/api/version
curl -s http://<esp-ip>/api/dashboard
curl -s http://<esp-ip>/api/pwr/base
curl -s -X POST http://<esp-ip>/api/pwr/refresh
curl -s -X POST "http://<esp-ip>/api/logfile/enable?enable=1"
