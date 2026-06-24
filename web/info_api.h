#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include "../wp_webserver.h"
#include "../py_parser_info.h"
#include "../config.h"
#include "../py_scheduler.h"
#include "../py_parser_pwr.h"
#include "api_cache.h"

struct InfoSpiRamAllocator {
    void* allocate(size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void* pointer) {
        heap_caps_free(pointer);
    }

    void* reallocate(void* ptr, size_t new_size) {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

using InfoSpiRamJsonDocument = BasicJsonDocument<InfoSpiRamAllocator>;

static void handleApiInfoValues();
static const size_t MIN_INFO_CACHE_HEADERS = 8;
static std::vector<InfoField> infoLastGoodFields;

static bool infoHasNonEmptyRawFields(const std::vector<InfoField>& fields) {
    for (const auto& f : fields) {
        String v = f.raw;
        v.trim();
        if (v.length() > 0) return true;
    }
    return false;
}

static bool infoJsonHasNonEmptyValue(const String& json) {
    int vIdx = json.indexOf("\"values\"");
    if (vIdx < 0) return false;
    int arrStart = json.indexOf('[', vIdx);
    int arrEnd = json.indexOf(']', arrStart > 0 ? arrStart : 0);
    if (arrStart < 0 || arrEnd < 0 || arrEnd <= arrStart) return false;

    bool inStr = false;
    bool hasChar = false;
    for (int i = arrStart + 1; i < arrEnd; i++) {
        char c = json[i];
        if (!inStr) {
            if (c == '"') {
                inStr = true;
                hasChar = false;
            }
            continue;
        }
        if (c == '\\') {
            if (i + 1 < arrEnd) {
                hasChar = true;
                i++;
            }
            continue;
        }
        if (c == '"') {
            if (hasChar) return true;
            inStr = false;
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            hasChar = true;
        }
    }
    return false;
}

static bool infoFieldsLookValid(const std::vector<InfoField>& fields) {
    if (fields.size() < 6) return false;

    for (const auto& f : fields) {
        if (f.name.length() < 2 || f.name.length() > 64) return false;
        char c0 = f.name[0];
        if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;
    }

    return true;
}

static bool infoCacheLooksValid(const String& json) {
    // Lightweight check WITHOUT full JSON parse.
    // Full deserialization was failing due to heap fragmentation, causing the cache
    // to be cleared on every menu return and making INFO appear empty.
    if (json.length() < 50) return false;
    if (json.indexOf("\"headers\"") < 0) return false;
    if (json.indexOf("\"values\"")  < 0) return false;
    if (json.indexOf("\"config\"")  < 0) return false;
    // headers array must not be empty
    int hIdx = json.indexOf("\"headers\"");
    int arrStart = json.indexOf('[', hIdx);
    int arrEnd   = json.indexOf(']', arrStart > 0 ? arrStart : 0);
    if (arrStart < 0 || arrEnd < 0 || arrEnd <= arrStart + 2) return false;
    if (!infoJsonHasNonEmptyValue(json)) return false;
    return true;
}

static void registerInfoAPI() {
    server.on("/api/info/values", HTTP_GET, handleApiInfoValues);
    server.on("/api/info/refresh", HTTP_POST, []() {
        apiCacheClear("info_vals");
        py_scheduler.triggerExclusiveInfo();
        server.send(200, "text/plain", "INFO refresh gestartet (alle Module)");
    });
    server.on("/api/info/set",    HTTP_POST, []() {

        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Missing body");
            return;
        }

        const size_t bodyLen = server.arg("plain").length();
        if (bodyLen > 120000) {
            server.send(413, "text/plain", "Payload too large");
            return;
        }

        InfoSpiRamJsonDocument req(24576);
        if (deserializeJson(req, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON or payload too large for parser");
            return;
        }

        // CONFIG
        config.battery.intervalInfo = req["config"]["intervalInfo"] | config.battery.intervalInfo;
        config.battery.enableInfo   = req["config"]["enableInfo"]   | config.battery.enableInfo;

        // MQTT
        config.mqtt.topicInfo = req["mqtt"]["topicInfo"] | config.mqtt.topicInfo;

        // FIELDS
        JsonArray arr = req["fields"];
        for (JsonObject f : arr) {

            String name = f["name"] | "";
            if (name.length() == 0) continue;

            if (!config.battery.fieldsInfo.count(name)) {
                FieldConfig fc;
                fc.label   = name;
                fc.display = name;
                fc.factor  = "1";
                fc.unit    = "";
                fc.mqtt    = false;
                fc.send    = false;
                config.battery.fieldsInfo[name] = fc;
            }

            FieldConfig &fc = config.battery.fieldsInfo[name];

            fc.display = f["display"]     | fc.display;
            fc.label   = f["display"]     | fc.label;
            fc.factor  = f["factor"]      | fc.factor;
            fc.unit    = f["unit"]        | fc.unit;
            fc.mqtt    = f["sendMQTT"]    | false;
            fc.send    = f["sendPayload"] | false;
        }

        discoveryInfoNeeded = true;

        // Nur die relevanten Abschnitte speichern (nicht config.save() um Heap-Druck zu vermeiden)
        config.saveSystemConfig();   // MQTT topicInfo
        config.saveInfoConfig();     // intervalInfo, enableInfo
        config.saveInfoFields();     // fieldsInfo
        apiCacheClear("info_vals");

        server.send(200, "text/plain", "INFO settings saved");
    });
}

static void handleApiInfoValues() {
    static unsigned long lastAutoInfoTrigger = 0;
    String statusText = "";

    String cached = apiCacheLoad("info_vals");
    if (cached.length() > 0) {
        if (infoCacheLooksValid(cached)) {
            // Inject status fields via string manipulation instead of full re-parse
            // (avoids double heap allocation that caused OOM and empty INFO page).
            String out = cached;
            int lastBrace = out.lastIndexOf('}');
            if (lastBrace > 0) {
                out = out.substring(0, lastBrace)
                    + ",\"statusText\":\"INFO aus NVS-Cache geladen\""
                    + ",\"dataSource\":\"cache\""
                    + "}";
            }
            server.send(200, "application/json", out);
            return;
        }
        // Invalid cached payload: clear and rebuild from fresh parser data.
        apiCacheClear("info_vals");
        statusText = "INFO-Cache war ungueltig und wurde verworfen";
    }

    // Schnelle Kopie unter Mutex, sofort freigeben – sendContent NIE unter Mutex!
    std::vector<InfoField> fields;
    bool usedRamSnapshot = false;
    bool usedConfigFallback = false;
    if (g_infoMutex && xSemaphoreTake(g_infoMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        fields = lastParsedInfo.fields;
        xSemaphoreGive(g_infoMutex);
    }

    // Fallback for menu-switch windows: use last known good INFO data from RAM.
    if (fields.empty() && !infoLastGoodFields.empty()) {
        fields = infoLastGoodFields;
        usedRamSnapshot = true;
    }

    // Strong fallback: last known good INFO payload stored in API cache.
    if (fields.empty()) {
        String lastGood = apiCacheLoad("info_last_good");
        if (infoCacheLooksValid(lastGood)) {
            String out = lastGood;
            int lastBrace = out.lastIndexOf('}');
            if (lastBrace > 0) {
                out = out.substring(0, lastBrace)
                    + ",\"statusText\":\"INFO aus stabilem Last-Good-Cache geladen\""
                    + ",\"dataSource\":\"cache\""
                    + "}";
            }
            server.send(200, "application/json", out);
            return;
        }
    }

    // Final fallback: build table from stored field settings so page is never empty.
    if (fields.empty() && !config.battery.fieldsInfo.empty()) {
        for (const auto& kv : config.battery.fieldsInfo) {
            InfoField f;
            f.name = kv.first;
            f.raw = "";
            fields.push_back(f);
        }
        usedConfigFallback = true;
    }

    if (!fields.empty() && infoFieldsLookValid(fields)) {
        infoLastGoodFields = fields;
    }

    InfoSpiRamJsonDocument doc(8192);
    JsonObject cfg = doc.createNestedObject("config");
    cfg["intervalInfo"] = config.battery.intervalInfo;
    cfg["enableInfo"] = config.battery.enableInfo;

    JsonObject mqtt = doc.createNestedObject("mqtt");
    mqtt["topicInfo"] = config.mqtt.topicInfo;

    JsonArray headers = doc.createNestedArray("headers");
    JsonArray values  = doc.createNestedArray("values");
    JsonArray fieldArr = doc.createNestedArray("fields");

    for (auto &pf : fields) {
        headers.add(pf.name);
        values.add(pf.raw);
        if (!config.battery.fieldsInfo.count(pf.name)) continue;
        const FieldConfig &f = config.battery.fieldsInfo.at(pf.name);
        JsonObject o = fieldArr.createNestedObject();
        o["name"] = pf.name;
        o["display"] = f.display;
        o["factor"] = f.factor;
        o["unit"] = f.unit;
        o["sendMQTT"] = f.mqtt;
        o["sendPayload"] = f.send;
        o["raw"] = pf.raw;
        o["value"] = pf.raw;
    }

    const bool hasLiveRaw = infoHasNonEmptyRawFields(fields);
    doc["cacheTimestamp"] = hasLiveRaw ? (uint32_t)millis() : 0;
    if (usedConfigFallback) {
        statusText = "INFO aus gespeicherten Feldern geladen";
    } else if (usedRamSnapshot) {
        statusText = "INFO aus RAM-Snapshot geladen";
    } else if (!fields.empty() && infoFieldsLookValid(fields)) {
        statusText = "INFO Live-Daten konsistent, Cache aktualisiert";
    } else {
        // Avoid spamming UART on every poll request.
        unsigned long now = millis();
        if (now - lastAutoInfoTrigger > 6000) {
            lastAutoInfoTrigger = now;
            py_scheduler.triggerExclusiveInfo();
            statusText = "INFO wird gerade per UART abgefragt (alle Module)";
        } else {
            statusText = "Warte auf naechsten INFO-Abfrageversuch (alle Module)";
        }

        if (infoLastStatusMessage.length() > 0) {
            statusText += " | Letzter Parser-Status: " + infoLastStatusMessage;
        }
    }

    if (statusText.length() == 0) statusText = "INFO bereit";
    doc["statusText"] = statusText;
    doc["dataSource"] = hasLiveRaw
                      ? ((usedConfigFallback || usedRamSnapshot) ? "cache" : "uart")
                      : "none";

    if (!fields.empty() && infoFieldsLookValid(fields) && hasLiveRaw) {
        String cacheOut;
        serializeJson(doc, cacheOut);
        apiCacheSave("info_vals", cacheOut);
        apiCacheSave("info_last_good", cacheOut);
    }

    server.setContentLength(measureJson(doc));
    server.send(200, "application/json", "");
    serializeJson(doc, server.client());
}
