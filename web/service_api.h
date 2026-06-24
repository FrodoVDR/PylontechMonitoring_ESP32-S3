#pragma once
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "service.h"
#include "../config.h"
#include "../py_systemmanager.h"
#include "../py_log.h"

void registerServiceAPI(WebServer &server) {

    // ---------------------------------------------------------
    // SERVICE PAGE
    // ---------------------------------------------------------
    server.on("/service", HTTP_GET, [&]() {
        server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate, max-age=0"));
        server.sendHeader(F("Pragma"), F("no-cache"));
        server.sendHeader(F("Expires"), F("0"));
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
            String msg = ok ? String("OK") : String("OTA Fehler: ") + Update.errorString();
            server.send(ok ? 200 : 500, F("text/plain"), msg);
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
                    Log(LOG_ERROR, String(F("OTA BeginFail: ")) + Update.errorString());
                    Update.abort();
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
        server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate, max-age=0"));
        server.sendHeader(F("Pragma"), F("no-cache"));
        server.sendHeader(F("Expires"), F("0"));
        server.send(200, F("text/plain"), config.getFirmwareVersionWithBuild());
    });

    // ---------------------------------------------------------
    // STORAGE INFO
    // ---------------------------------------------------------
    server.on("/api/storageinfo", HTTP_GET, [&]() {

        size_t spiffsTotal = SPIFFS.totalBytes();
        size_t spiffsUsed  = SPIFFS.usedBytes();
        size_t spiffsFree  = spiffsTotal - spiffsUsed;

        // NVS stats – ensure NVS is initialized before querying
        nvs_stats_t st = {};
        bool nvsOk = false;
        esp_err_t err = nvs_flash_init();
        if (err == ESP_OK || err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            if (err != ESP_OK) {
                nvs_flash_erase();
                nvs_flash_init();
            }
            err = nvs_get_stats("nvs", &st);
            if (err != ESP_OK) {
                err = nvs_get_stats(NULL, &st);
            }
            nvsOk = (err == ESP_OK);
        }

        String json = "{";
        json += "\"spiffs_total\":" + String(spiffsTotal) + ",";
        json += "\"spiffs_used\":"  + String(spiffsUsed)  + ",";
        json += "\"spiffs_free\":"  + String(spiffsFree);
        if (nvsOk) {
            json += ",\"nvs_total\":" + String(st.total_entries);
            json += ",\"nvs_used\":"  + String(st.used_entries);
            json += ",\"nvs_free\":"  + String(st.free_entries);
        }
        json += "}";

        server.send(200, F("application/json"), json);
    });

    // ---------------------------------------------------------
    // LOGFILE STATUS (SPIFFS)
    // ---------------------------------------------------------
    server.on("/api/logfile/status", HTTP_GET, [&]() {
        String json = "{";
        json += "\"enabled\":" + String(WebLogFileEnabled() ? "true" : "false") + ",";
        json += "\"path\":\"" + WebLogFilePath() + "\"";
        json += "}";
        server.send(200, F("application/json"), json);
    });

    // ---------------------------------------------------------
    // LOGFILE ENABLE/DISABLE
    // ---------------------------------------------------------
    server.on("/api/logfile/enable", HTTP_POST, [&]() {
        bool enable = false;
        if (server.hasArg("enable")) {
            String v = server.arg("enable");
            enable = (v == "1" || v == "true" || v == "on");
        }
        WebLogFileEnable(enable);
        server.send(200, F("text/plain"), enable ? F("Logfile enabled") : F("Logfile disabled"));
    });

    // ---------------------------------------------------------
    // LOGFILE CLEAR
    // ---------------------------------------------------------
    server.on("/api/logfile/clear", HTTP_POST, [&]() {
        bool ok = WebLogFileClear();
        server.send(ok ? 200 : 500, F("text/plain"), ok ? F("Logfile cleared") : F("Logfile clear failed"));
    });

    // ---------------------------------------------------------
    // SETTINGS BACKUP  (GET → JSON download)
    // ---------------------------------------------------------
    server.on("/api/backup", HTTP_GET, [&]() {
        DynamicJsonDocument doc(16384);

        doc["deviceName"]   = config.deviceName;
        doc["hostname"]     = config.hostname;
        doc["wifiSSID"]     = config.wifiSSID;
        // wifiPass intentionally excluded for security
        doc["apSSID"]       = config.apSSID;
        doc["apPass"]       = config.apPass;

        // WiFi static IP
        doc["useStaticIP"]  = config.useStaticIP;
        doc["ipAddr"]       = config.ipAddr;
        doc["subnetMask"]   = config.subnetMask;
        doc["gateway"]      = config.gateway;
        doc["dns"]          = config.dns;

        // Ethernet
        doc["useEthernet"]    = config.useEthernet;
        doc["ethPhyAddr"]     = config.ethPhyAddr;
        doc["ethMisoPin"]     = config.ethMisoPin;
        doc["ethMosiPin"]     = config.ethMosiPin;
        doc["ethSclkPin"]     = config.ethSclkPin;
        doc["ethCsPin"]       = config.ethCsPin;
        doc["ethRstPin"]      = config.ethRstPin;
        doc["ethIntPin"]      = config.ethIntPin;
        doc["ethUseStaticIP"] = config.ethUseStaticIP;
        doc["ethIpAddr"]      = config.ethIpAddr;
        doc["ethSubnetMask"]  = config.ethSubnetMask;
        doc["ethGateway"]     = config.ethGateway;
        doc["ethDns"]         = config.ethDns;

        // NTP / Time
        doc["ntpServer"]          = config.ntpServer;
        doc["timezone"]           = config.timezone;
        doc["use_gateway_ntp"]    = config.use_gateway_ntp;
        doc["manual_ntp"]         = config.manual_ntp;
        doc["manual_mode"]        = config.manual_mode;
        doc["manual_dst"]         = config.manual_dst;
        doc["daylightSaving"]     = config.daylightSaving;
        doc["ntpResyncInterval"]  = config.ntpResyncInterval;

        // MQTT
        doc["mqtt_enabled"]   = config.mqtt.enabled;
        doc["mqtt_server"]    = config.mqtt.server;
        doc["mqtt_port"]      = config.mqtt.port;
        doc["mqtt_user"]      = config.mqtt.user;
        // mqtt_pass excluded for security
        doc["mqtt_prefix"]    = config.mqtt.prefix;
        doc["mqtt_topicStack"]= config.mqtt.topicStack;
        doc["mqtt_topicPwr"]  = config.mqtt.topicPwr;
        doc["mqtt_topicBat"]  = config.mqtt.topicBat;
        doc["mqtt_topicStat"] = config.mqtt.topicStat;
        doc["mqtt_topicInfo"] = config.mqtt.topicInfo;
        doc["mqtt_cellPrefix"]= config.mqtt.cellPrefix;
        doc["mqtt_mode"]      = config.mqtt.mode;

        // Battery
        doc["bat_intervalPwr"]   = config.battery.intervalPwr;
        doc["bat_intervalBat"]   = config.battery.intervalBat;
        doc["bat_intervalStat"]  = config.battery.intervalStat;
        doc["bat_intervalInfo"]  = config.battery.intervalInfo;
        doc["bat_enableBat"]     = config.battery.enableBat;
        doc["bat_enableStat"]    = config.battery.enableStat;
        doc["bat_enableInfo"]    = config.battery.enableInfo;
        doc["bat_useFahrenheit"] = config.battery.useFahrenheit;
        doc["bat_maxModules"]    = config.battery.maxModules;
        doc["bat_cellDiffWarn"]  = config.battery.cellDiffWarn;
        doc["bat_cellDiffError"] = config.battery.cellDiffError;

        // Field configurations (PWR / BAT / STAT / INFO)
        JsonArray fPwr = doc.createNestedArray("fieldsPwr");
        for (auto &kv : config.battery.fieldsPwr) {
            JsonObject o = fPwr.createNestedObject();
            o["name"]    = kv.first;
            o["display"] = kv.second.display;
            o["factor"]  = kv.second.factor;
            o["unit"]    = kv.second.unit;
            o["mqtt"]    = kv.second.mqtt;
            o["send"]    = kv.second.send;
        }
        JsonArray fBat = doc.createNestedArray("fieldsBat");
        for (auto &kv : config.battery.fieldsBat) {
            JsonObject o = fBat.createNestedObject();
            o["name"]    = kv.first;
            o["display"] = kv.second.display;
            o["factor"]  = kv.second.factor;
            o["unit"]    = kv.second.unit;
            o["mqtt"]    = kv.second.mqtt;
            o["send"]    = kv.second.send;
        }
        JsonArray fStat = doc.createNestedArray("fieldsStat");
        for (auto &kv : config.battery.fieldsStat) {
            JsonObject o = fStat.createNestedObject();
            o["name"]    = kv.first;
            o["display"] = kv.second.display;
            o["factor"]  = kv.second.factor;
            o["unit"]    = kv.second.unit;
            o["mqtt"]    = kv.second.mqtt;
            o["send"]    = kv.second.send;
        }
        JsonArray fInfo = doc.createNestedArray("fieldsInfo");
        for (auto &kv : config.battery.fieldsInfo) {
            JsonObject o = fInfo.createNestedObject();
            o["name"]    = kv.first;
            o["display"] = kv.second.display;
            o["factor"]  = kv.second.factor;
            o["unit"]    = kv.second.unit;
            o["mqtt"]    = kv.second.mqtt;
            o["send"]    = kv.second.send;
        }

        // Logging
        doc["logInfo"]  = config.logInfo;
        doc["logWarn"]  = config.logWarn;
        doc["logError"] = config.logError;
        doc["logDebug"] = config.logDebug;
        doc["syslogEnabled"] = config.syslogEnabled;
        doc["syslogServer"]  = config.syslogServer;
        doc["syslogPort"]    = config.syslogPort;

        String out;
        serializeJsonPretty(doc, out);

        server.sendHeader("Content-Disposition", "attachment; filename=\"pylontech_backup.json\"");
        server.send(200, F("application/json"), out);

        Log(LOG_INFO, F("Backup downloaded"));
    });

    // ---------------------------------------------------------
    // SETTINGS RESTORE  (POST ← JSON upload)
    // ---------------------------------------------------------
    server.on("/api/restore", HTTP_POST, [&]() {
        if (!server.hasArg("plain"))
            return server.send(400, F("text/plain"), F("Missing body"));

        DynamicJsonDocument doc(16384);
        if (deserializeJson(doc, server.arg("plain"))) {
            return server.send(400, F("text/plain"), F("Invalid JSON"));
        }

        if (doc.containsKey("deviceName"))   config.deviceName   = doc["deviceName"].as<String>();
        // Keep hostname/AP-SSID device-local: do not import these from backup.
        if (doc.containsKey("wifiSSID"))     config.wifiSSID     = doc["wifiSSID"].as<String>();
        if (doc.containsKey("apPass"))       config.apPass       = doc["apPass"].as<String>();

        if (doc.containsKey("useStaticIP"))  config.useStaticIP  = doc["useStaticIP"];
        if (doc.containsKey("ipAddr"))       config.ipAddr       = doc["ipAddr"].as<String>();
        if (doc.containsKey("subnetMask"))   config.subnetMask   = doc["subnetMask"].as<String>();
        if (doc.containsKey("gateway"))      config.gateway      = doc["gateway"].as<String>();
        if (doc.containsKey("dns"))          config.dns          = doc["dns"].as<String>();

        if (doc.containsKey("useEthernet"))    config.useEthernet    = doc["useEthernet"];
        if (doc.containsKey("ethPhyAddr"))     config.ethPhyAddr     = doc["ethPhyAddr"];
        if (doc.containsKey("ethMisoPin"))     config.ethMisoPin     = doc["ethMisoPin"];
        if (doc.containsKey("ethMosiPin"))     config.ethMosiPin     = doc["ethMosiPin"];
        if (doc.containsKey("ethSclkPin"))     config.ethSclkPin     = doc["ethSclkPin"];
        if (doc.containsKey("ethCsPin"))       config.ethCsPin       = doc["ethCsPin"];
        if (doc.containsKey("ethRstPin"))      config.ethRstPin      = doc["ethRstPin"];
        if (doc.containsKey("ethIntPin"))      config.ethIntPin      = doc["ethIntPin"];
        if (doc.containsKey("ethUseStaticIP")) config.ethUseStaticIP = doc["ethUseStaticIP"];
        if (doc.containsKey("ethIpAddr"))      config.ethIpAddr      = doc["ethIpAddr"].as<String>();
        if (doc.containsKey("ethSubnetMask"))  config.ethSubnetMask  = doc["ethSubnetMask"].as<String>();
        if (doc.containsKey("ethGateway"))     config.ethGateway     = doc["ethGateway"].as<String>();
        if (doc.containsKey("ethDns"))         config.ethDns         = doc["ethDns"].as<String>();

        if (doc.containsKey("ntpServer"))         config.ntpServer         = doc["ntpServer"].as<String>();
        if (doc.containsKey("timezone"))          config.timezone          = doc["timezone"].as<String>();
        if (doc.containsKey("use_gateway_ntp"))   config.use_gateway_ntp   = doc["use_gateway_ntp"];
        if (doc.containsKey("manual_ntp"))        config.manual_ntp        = doc["manual_ntp"];
        if (doc.containsKey("manual_mode"))       config.manual_mode       = doc["manual_mode"];
        if (doc.containsKey("manual_dst"))        config.manual_dst        = doc["manual_dst"];
        if (doc.containsKey("daylightSaving"))    config.daylightSaving    = doc["daylightSaving"];
        if (doc.containsKey("ntpResyncInterval")) config.ntpResyncInterval = doc["ntpResyncInterval"];

        if (doc.containsKey("mqtt_enabled"))    config.mqtt.enabled    = doc["mqtt_enabled"];
        if (doc.containsKey("mqtt_server"))     config.mqtt.server     = doc["mqtt_server"].as<String>();
        if (doc.containsKey("mqtt_port"))       config.mqtt.port       = doc["mqtt_port"];
        if (doc.containsKey("mqtt_user"))       config.mqtt.user       = doc["mqtt_user"].as<String>();
        if (doc.containsKey("mqtt_prefix"))     config.mqtt.prefix     = doc["mqtt_prefix"].as<String>();
        if (doc.containsKey("mqtt_topicStack")) config.mqtt.topicStack = doc["mqtt_topicStack"].as<String>();
        if (doc.containsKey("mqtt_topicPwr"))   config.mqtt.topicPwr   = doc["mqtt_topicPwr"].as<String>();
        if (doc.containsKey("mqtt_topicBat"))   config.mqtt.topicBat   = doc["mqtt_topicBat"].as<String>();
        if (doc.containsKey("mqtt_topicStat"))  config.mqtt.topicStat  = doc["mqtt_topicStat"].as<String>();
        if (doc.containsKey("mqtt_topicInfo"))  config.mqtt.topicInfo  = doc["mqtt_topicInfo"].as<String>();
        if (doc.containsKey("mqtt_cellPrefix")) config.mqtt.cellPrefix = doc["mqtt_cellPrefix"].as<String>();
        if (doc.containsKey("mqtt_mode"))       config.mqtt.mode       = doc["mqtt_mode"].as<String>();

        if (doc.containsKey("bat_intervalPwr"))   config.battery.intervalPwr   = doc["bat_intervalPwr"];
        if (doc.containsKey("bat_intervalBat"))   config.battery.intervalBat   = doc["bat_intervalBat"];
        if (doc.containsKey("bat_intervalStat"))  config.battery.intervalStat  = doc["bat_intervalStat"];
        if (doc.containsKey("bat_intervalInfo"))  config.battery.intervalInfo  = doc["bat_intervalInfo"];
        if (doc.containsKey("bat_enableBat"))     config.battery.enableBat     = doc["bat_enableBat"];
        if (doc.containsKey("bat_enableStat"))    config.battery.enableStat    = doc["bat_enableStat"];
        if (doc.containsKey("bat_enableInfo"))    config.battery.enableInfo    = doc["bat_enableInfo"];
        if (doc.containsKey("bat_useFahrenheit")) config.battery.useFahrenheit = doc["bat_useFahrenheit"];
        if (doc.containsKey("bat_maxModules"))    config.battery.maxModules    = doc["bat_maxModules"];
        if (doc.containsKey("bat_cellDiffWarn"))  config.battery.cellDiffWarn  = doc["bat_cellDiffWarn"];
        if (doc.containsKey("bat_cellDiffError")) config.battery.cellDiffError = doc["bat_cellDiffError"];

        if (doc.containsKey("logInfo"))  config.logInfo  = doc["logInfo"];
        if (doc.containsKey("logWarn"))  config.logWarn  = doc["logWarn"];
        if (doc.containsKey("logError")) config.logError = doc["logError"];
        if (doc.containsKey("logDebug")) config.logDebug = doc["logDebug"];
        if (doc.containsKey("syslogEnabled")) config.syslogEnabled = doc["syslogEnabled"];
        if (doc.containsKey("syslogServer"))  config.syslogServer  = doc["syslogServer"].as<String>();
        if (doc.containsKey("syslogPort"))    config.syslogPort    = doc["syslogPort"];

        // Restore field configurations (PWR / BAT / STAT / INFO)
        if (doc.containsKey("fieldsPwr")) {
            config.battery.fieldsPwr.clear();
            for (JsonObject f : doc["fieldsPwr"].as<JsonArray>()) {
                String name = f["name"] | "";
                if (name.isEmpty()) continue;
                FieldConfig fc;
                fc.label   = f["display"] | name;
                fc.display = f["display"] | name;
                fc.factor  = f["factor"]  | "1";
                fc.unit    = f["unit"]    | "";
                fc.mqtt    = f["mqtt"]    | false;
                fc.send    = f["send"]    | false;
                config.battery.fieldsPwr[name] = fc;
            }
        }
        if (doc.containsKey("fieldsBat")) {
            config.battery.fieldsBat.clear();
            for (JsonObject f : doc["fieldsBat"].as<JsonArray>()) {
                String name = f["name"] | "";
                if (name.isEmpty()) continue;
                FieldConfig fc;
                fc.label   = f["display"] | name;
                fc.display = f["display"] | name;
                fc.factor  = f["factor"]  | "1";
                fc.unit    = f["unit"]    | "";
                fc.mqtt    = f["mqtt"]    | false;
                fc.send    = f["send"]    | false;
                config.battery.fieldsBat[name] = fc;
            }
        }
        if (doc.containsKey("fieldsStat")) {
            config.battery.fieldsStat.clear();
            for (JsonObject f : doc["fieldsStat"].as<JsonArray>()) {
                String name = f["name"] | "";
                if (name.isEmpty()) continue;
                FieldConfig fc;
                fc.label   = f["display"] | name;
                fc.display = f["display"] | name;
                fc.factor  = f["factor"]  | "1";
                fc.unit    = f["unit"]    | "";
                fc.mqtt    = f["mqtt"]    | false;
                fc.send    = f["send"]    | false;
                config.battery.fieldsStat[name] = fc;
            }
        }
        if (doc.containsKey("fieldsInfo")) {
            config.battery.fieldsInfo.clear();
            for (JsonObject f : doc["fieldsInfo"].as<JsonArray>()) {
                String name = f["name"] | "";
                if (name.isEmpty()) continue;
                FieldConfig fc;
                fc.label   = f["display"] | name;
                fc.display = f["display"] | name;
                fc.factor  = f["factor"]  | "1";
                fc.unit    = f["unit"]    | "";
                fc.mqtt    = f["mqtt"]    | false;
                fc.send    = f["send"]    | false;
                config.battery.fieldsInfo[name] = fc;
            }
        }

        config.save();
        Log(LOG_INFO, F("Settings restored from backup"));
        server.send(200, F("text/plain"), F("Settings restored – reboot to apply"));
    });
}
