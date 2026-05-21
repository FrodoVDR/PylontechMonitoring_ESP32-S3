#pragma once
#include "api_core.h"

#include <SPIFFS.h>
#include <WiFi.h>

#include "../py_mqtt.h"
#include "../py_parser_pwr.h"
#include "../py_log.h"
#include "../config.h"
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

        // System
        time_t now; time(&now);
        struct tm t; localtime_r(&now, &t);
        char buf[32]; strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);

        sendChunk(F("\"system\":{"));
        sendChunk("\"time\":\"" + String(buf) + "\",");
        sendChunk("\"uptime\":\"" + config.uptimeString() + "\",");
        sendChunk("\"version\":\"" + config.firmwareVersion + "\"");
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

        String path = "/" + server.arg("file");
        if (!SPIFFS.exists(path))
            return apiError(404, F("File not found"));

        SPIFFS.remove(path);
        apiText(F("OK"));
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
        StaticJsonDocument<128> doc;
        doc["info"]  = config.logInfo;
        doc["warn"]  = config.logWarn;
        doc["error"] = config.logError;
        doc["debug"] = config.logDebug;

        String out;
        serializeJson(doc, out);
        apiJson(out);
    });

    apiPost("/api/log/level", []() {
        if (!server.hasArg("plain"))
            return apiError(400, F("Missing body"));

        StaticJsonDocument<128> req;
        if (deserializeJson(req, server.arg("plain")))
            return apiError(400, F("Invalid JSON"));

        config.logInfo  = req["info"]  | true;
        config.logWarn  = req["warn"]  | true;
        config.logError = req["error"] | true;
        config.logDebug = req["debug"] | false;

        config.save();
        apiText(F("Log level updated"));
    });
}
