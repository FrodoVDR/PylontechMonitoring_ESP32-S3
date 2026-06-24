#pragma once
#include "api_core.h"

#include <SPIFFS.h>
#include <WiFi.h>
#include <Preferences.h>

#include "../py_mqtt.h"
#include "../py_parser_pwr.h"
#include "../py_log.h"
#include "../config.h"
#include "../py_eth.h"
#include "../py_wifimanager.h"
#include "filemanager.h"

extern AppConfig config;
extern PyMqtt py_mqtt;
extern BatteryStack lastParsedStack;

// Hilfsfunktion für Chunked JSON
static inline void sendChunk(const String &s) {
    server.sendContent(s);
}

inline void registerCombinedAPI() {

    // ---------------------------------------------------------
    // 1) Dashboard API
    // ---------------------------------------------------------
    apiGet("/api/dashboard", []() {

        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, F("application/json"), "");
        sendChunk(F("{"));

        // WiFi
        sendChunk(F("\"wifi\":{"));
        sendChunk(F("\"mode\":\"STA\","));
        sendChunk("\"ssid\":\"" + WiFi.SSID() + "\",");
        sendChunk("\"ip\":\"" + WiFi.localIP().toString() + "\",");
        sendChunk("\"mac\":\"" + WiFi.macAddress() + "\",");
        sendChunk("\"rssi\":" + String(WiFi.RSSI()));
        sendChunk(F("},"));

        // MQTT
        sendChunk(F("\"mqtt\":{"));
        sendChunk("\"connected\":" + String(py_mqtt.isConnected() ? "true" : "false") + ",");
        sendChunk("\"server\":\"" + config.mqtt.server + "\",");
        sendChunk("\"port\":" + String(config.mqtt.port) + ",");
        sendChunk("\"last_contact\":\"" + config.lastMqttContact + "\"");
        sendChunk(F("},"));

        // Battery
        sendChunk(F("\"battery\":{"));
        sendChunk("\"modules\":" + String(lastParsedStack.batteryCount) + ",");
        sendChunk("\"last_update\":\"" + config.lastPwrUpdate + "\"");
        sendChunk(F("},"));

        // System (calculate local time directly using offset)
        time_t now; time(&now);
        
        // Get UTC time
        struct tm utc_t; gmtime_r(&now, &utc_t);
        
        // Calculate local time by applying offset
        int offsetHours = config.getTimezoneOffsetHours();
        time_t local_time = now + (offsetHours * 3600);
        struct tm t; gmtime_r(&local_time, &t);
        
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        char utc_buf[32]; strftime(utc_buf, sizeof(utc_buf), "%Y-%m-%d %H:%M:%S", &utc_t);

        sendChunk(F("\"system\":{"));
        sendChunk("\"time\":\"" + String(buf) + "\",");
        sendChunk("\"timestamp\":" + String(now) + ",");
        sendChunk("\"utc\":\"" + String(utc_buf) + "\",");
        sendChunk("\"offset\":" + String(offsetHours) + ",");
        sendChunk("\"timezone\":\"" + config.timezone + "\",");
        sendChunk("\"uptime\":\"" + config.uptimeString() + "\",");
        sendChunk("\"version\":\"" + config.firmwareVersion + "\"");
        sendChunk(F("}"));

        // LAN (Ethernet)
        EthStatus eth = EthManagerModule::getStatus();
        sendChunk(F(",\"lan\":{"));
        sendChunk("\"enabled\":" + String(config.useEthernet ? "true" : "false") + ",");
        sendChunk("\"linked\":"    + String(eth.linked    ? "true" : "false") + ",");
        sendChunk("\"connected\":" + String(eth.connected ? "true" : "false") + ",");
        sendChunk("\"ip\":\""    + eth.ip    + "\",");
        sendChunk("\"mac\":\""   + eth.mac   + "\",");
        sendChunk("\"speed\":"   + String(eth.speed));
        sendChunk(F("}"));

        sendChunk(F("}"));
        sendChunk("");
    });

    // ---------------------------------------------------------
    // 2) Filemanager API
    // ---------------------------------------------------------
    apiGet("/filemanager", []() {
        server.setContentLength(strlen_P(FILEMANAGER_PAGE));
        server.send(200, F("text/html"), "");
        server.sendContent_P(FILEMANAGER_PAGE);
        server.sendContent("");
    });

    apiGet("/fm/list", []() {
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, F("application/json"), "[");

        File root = SPIFFS.open("/");
        File f = root.openNextFile();
        bool first = true;

        while (f) {
            if (!first) server.sendContent(",");
            first = false;

            server.sendContent("{\"name\":\"");
            server.sendContent(f.name());
            server.sendContent("\",\"size\":");
            server.sendContent(String(f.size()));
            server.sendContent("}");

            f = root.openNextFile();
        }

        server.sendContent("]");
        server.sendContent("");
    });

    apiGet("/fm/delete", []() {
        if (!server.hasArg("file"))
            return apiError(400, F("Missing file"));

        String path = server.arg("file");
        if (!path.startsWith("/")) path = "/" + path;
        if (!SPIFFS.exists(path))
            return apiError(404, F("File not found"));

        SPIFFS.remove(path);
        apiText(F("OK"));
    });

    apiGet("/fm/download", []() {
        if (!server.hasArg("file"))
            return apiError(400, F("Missing file"));

        String path = server.arg("file");
        String filename = path;
        if (filename.startsWith("/")) filename = filename.substring(1);
        if (!path.startsWith("/")) path = "/" + path;

        File f = SPIFFS.open(path, "r");
        if (!f)
            return apiError(404, F("File not found"));

        server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        server.streamFile(f, "application/octet-stream");
        f.close();
    });

    server.on("/fm/upload", HTTP_POST,
        []() { apiText(F("OK")); },
        []() {
            HTTPUpload &u = server.upload();
            static File f;

            if (u.status == UPLOAD_FILE_START) {
                f = SPIFFS.open("/" + u.filename, "w");
            }
            else if (u.status == UPLOAD_FILE_WRITE) {
                if (f) f.write(u.buf, u.currentSize);
            }
            else if (u.status == UPLOAD_FILE_END) {
                if (f) f.close();
            }
        }
    );

    // ---------------------------------------------------------
    // 3) Runtime API
    // ---------------------------------------------------------
    apiGet("/api/log", []() {
        apiText(WebLogGet());
    });

    apiGet("/api/log/level", []() {
        StaticJsonDocument<256> doc;
        doc["info"]  = config.logInfo;
        doc["warn"]  = config.logWarn;
        doc["error"] = config.logError;
        doc["debug"] = config.logDebug;
        doc["syslogEnabled"] = config.syslogEnabled;
        doc["syslogServer"]  = config.syslogServer;
        doc["syslogPort"]    = config.syslogPort;

        String out;
        serializeJson(doc, out);
        apiJson(out);
    });

    apiPost("/api/log/level", []() {
        if (!server.hasArg("plain"))
            return apiError(400, F("Missing body"));

        StaticJsonDocument<256> req;
        if (deserializeJson(req, server.arg("plain")))
            return apiError(400, F("Invalid JSON"));

        if (req.containsKey("info"))  config.logInfo  = req["info"];
        if (req.containsKey("warn"))  config.logWarn  = req["warn"];
        if (req.containsKey("error")) config.logError = req["error"];
        if (req.containsKey("debug")) config.logDebug = req["debug"];

        if (req.containsKey("syslogEnabled")) config.syslogEnabled = req["syslogEnabled"];
        if (req.containsKey("syslogServer"))  config.syslogServer  = req["syslogServer"].as<String>();
        if (req.containsKey("syslogPort"))    config.syslogPort    = req["syslogPort"];

        bool writeOk = true;
        Preferences p;
        p.begin("config", false);
        writeOk &= (p.putBool("log_info", config.logInfo) > 0);
        writeOk &= (p.putBool("log_warn", config.logWarn) > 0);
        writeOk &= (p.putBool("log_error", config.logError) > 0);
        writeOk &= (p.putBool("log_debug", config.logDebug) > 0);
        writeOk &= (p.putBool("syslog_en", config.syslogEnabled) > 0);

        size_t syslogLen = p.putString("syslog_srv", config.syslogServer);
        if (syslogLen != config.syslogServer.length()) {
            // Retry once after key cleanup in case NVS update kept a stale value.
            p.remove("syslog_srv");
            syslogLen = p.putString("syslog_srv", config.syslogServer);
        }
        writeOk &= (syslogLen == config.syslogServer.length());
        writeOk &= (p.putUShort("syslog_port", config.syslogPort) > 0);
        p.end();

        Preferences verify;
        verify.begin("config", true);
        const bool enNv = verify.getBool("syslog_en", false);
        const String srvNv = verify.getString("syslog_srv", "");
        const uint16_t portNv = verify.getUShort("syslog_port", 0);
        verify.end();

        if (!writeOk || enNv != config.syslogEnabled || srvNv != config.syslogServer || portNv != config.syslogPort) {
            Log(LOG_ERROR,
                String("Syslog persist failed: mem=") +
                (config.syslogEnabled ? "1" : "0") + "," + config.syslogServer + "," + String(config.syslogPort) +
                " nvs=" + (enNv ? "1" : "0") + "," + srvNv + "," + String(portNv));
            return apiError(500, F("Persist failed"));
        }

        apiText(F("Log level updated"));
    });
}
