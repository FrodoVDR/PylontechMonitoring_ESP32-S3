#pragma once
#include <WebServer.h>
#include <Update.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "service.h"
#include "../config.h"
#include "../py_systemmanager.h"

void registerServiceAPI(WebServer &server) {

    // ---------------------------------------------------------
    // SERVICE PAGE
    // ---------------------------------------------------------
    server.on("/service", HTTP_GET, [&]() {
        server.setContentLength(strlen_P(SERVICE_PAGE));
        server.send(200, F("text/html"), "");
        server.sendContent_P(SERVICE_PAGE);
        server.sendContent("");
    });

    // ---------------------------------------------------------
    // RESTART
    // ---------------------------------------------------------
    server.on("/api/restart", HTTP_POST, [&]() {
        Log(LOG_WARN, F("Restart"));
        server.send(200, F("text/plain"), F("Restarting"));
        delay(200);
        ESP.restart();
    });

    // ---------------------------------------------------------
    // FACTORY RESET
    // ---------------------------------------------------------
    server.on("/api/factoryreset", HTTP_POST, [&]() {
        Log(LOG_ERROR, F("FactoryReset"));
        server.send(200, F("text/plain"), F("Factory reset"));
        delay(200);
        SystemManager::triggerFactoryReset();
    });

    // ---------------------------------------------------------
    // WIFI RESET
    // ---------------------------------------------------------
    server.on("/api/wifireset", HTTP_POST, [&]() {
        Log(LOG_WARN, F("WiFiReset"));
        server.send(200, F("text/plain"), F("WiFi reset"));
        delay(200);
        SystemManager::triggerWiFiReset();
    });

    // ---------------------------------------------------------
    // OTA UPLOAD
    // ---------------------------------------------------------
    server.on("/api/ota", HTTP_POST,

        // OTA FINISH
        [&]() {
            bool ok = !Update.hasError();
            Log(ok ? LOG_INFO : LOG_ERROR, ok ? F("OTA OK") : F("OTA FAIL"));
            server.send(ok ? 200 : 500, F("text/plain"), ok ? F("OK") : F("OTA Fehler"));
            delay(200);
            ESP.restart();
        },

        // OTA UPLOAD CALLBACK
        [&]() {
            HTTPUpload &u = server.upload();
            static size_t total = 0;

            if (u.status == UPLOAD_FILE_START) {

                total = 0;
                Log(LOG_INFO, F("OTA Start"));

                String name = u.filename;

                // Dateiname prüfen
                if (!name.endsWith(".bin") || name.indexOf("Pylontech") < 0) {
                    Log(LOG_ERROR, F("OTA BadName"));
                    server.send(400, F("text/plain"),
                        F("Ungültige Datei (.bin + 'Pylontech' nötig)"));
                    Update.abort();
                    return;
                }

                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Log(LOG_ERROR, F("OTA BeginFail"));
                    return;
                }
            }

            else if (u.status == UPLOAD_FILE_WRITE) {

                total += u.currentSize;

                const size_t MAX_SIZE = 2 * 1024 * 1024;
                if (total > MAX_SIZE) {
                    Log(LOG_ERROR, F("OTA TooBig"));
                    server.send(400, F("text/plain"), F("Firmware-Datei zu groß"));
                    Update.abort();
                    return;
                }

                Update.write(u.buf, u.currentSize);
            }

            else if (u.status == UPLOAD_FILE_END) {

                const size_t MIN_SIZE = 100 * 1024;
                if (total < MIN_SIZE) {
                    Log(LOG_ERROR, F("OTA TooSmall"));
                    server.send(400, F("text/plain"), F("Firmware-Datei zu klein"));
                    Update.abort();
                    return;
                }

                Log(LOG_INFO, F("OTA End"));
                Update.end(true);
            }

            else if (u.status == UPLOAD_FILE_ABORTED) {
                Log(LOG_WARN, F("OTA Abort"));
                Update.abort();
            }
        }
    );

    // ---------------------------------------------------------
    // VERSION
    // ---------------------------------------------------------
    server.on("/api/version", HTTP_GET, [&]() {
        server.send(200, F("text/plain"), config.firmwareVersion);
    });

    // ---------------------------------------------------------
    // STORAGE INFO
    // ---------------------------------------------------------
    server.on("/api/storageinfo", HTTP_GET, [&]() {

        size_t spiffsTotal = SPIFFS.totalBytes();
        size_t spiffsUsed  = SPIFFS.usedBytes();
        size_t spiffsFree  = spiffsTotal - spiffsUsed;

        nvs_stats_t st;
        nvs_get_stats(NULL, &st);

        String json = "{";
        json += "\"spiffs_total\":" + String(spiffsTotal) + ",";
        json += "\"spiffs_used\":"  + String(spiffsUsed)  + ",";
        json += "\"spiffs_free\":"  + String(spiffsFree)  + ",";
        json += "\"nvs_total\":"    + String(st.total_entries) + ",";
        json += "\"nvs_used\":"     + String(st.used_entries)  + ",";
        json += "\"nvs_free\":"     + String(st.free_entries);
        json += "}";

        server.send(200, F("application/json"), json);
    });
}
