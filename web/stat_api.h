#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include "../wp_webserver.h"
#include "../py_parser_stat.h"
#include "../config.h"
#include "../py_scheduler.h"
#include "../py_parser_pwr.h"
#include "api_cache.h"

struct SpiRamAllocator {
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

using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;

static void handleApiStatValues();
static void handleApiStatSet();

static bool statKeyLooksPlausibleApi(const String& key) {
    if (key.length() < 2 || key.length() > 64) return false;
    char c0 = key[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;
    if (key.indexOf('$') >= 0 || key.indexOf('@') >= 0) return false;
    return true;
}

static void registerStatAPI() {
    server.on("/api/stat/values",  HTTP_GET,  handleApiStatValues);
    server.on("/api/stat/set",     HTTP_POST, handleApiStatSet);
    server.on("/api/stat/refresh", HTTP_POST, []() {
        apiCacheClear("stat_vals");
        py_scheduler.triggerExclusiveStat();
        server.send(200, "text/plain", "STAT refresh gestartet (alle Module)");
    });
}

static void handleApiStatValues() {
    static unsigned long lastAutoStatTrigger = 0;

    String cached = apiCacheLoad("stat_vals");
    if (cached.length() > 0) {
        // Lightweight validation: avoid full deserialize which can OOM and silently discard cache.
        bool valid = (cached.length() >= 50
            && cached.indexOf("\"headers\"") >= 0
            && cached.indexOf("\"values\"")  >= 0);

        if (valid) {
            // Inject status fields via string instead of full re-parse
            String out = cached;
            int lastBrace = out.lastIndexOf('}');
            if (lastBrace > 0) {
                out = out.substring(0, lastBrace)
                    + ",\"statusText\":\"STAT aus NVS-Cache geladen\""
                    + ",\"dataSource\":\"cache\""
                    + "}";
            }
            server.send(200, "application/json", out);
            return;
        }
        apiCacheClear("stat_vals");
    }

    // Schnelle Kopie unter Mutex, sofort freigeben – sendContent NIE unter Mutex!
    std::vector<StatField> fields;
    if (g_statMutex && xSemaphoreTake(g_statMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        fields = lastParsedStat.fields;
        xSemaphoreGive(g_statMutex);
    }

    SpiRamJsonDocument doc(16384);
    JsonObject cfg = doc.createNestedObject("config");
    cfg["intervalStat"] = config.battery.intervalStat;
    cfg["enableStat"] = config.battery.enableStat;

    JsonObject mqtt = doc.createNestedObject("mqtt");
    mqtt["topicStat"] = config.mqtt.topicStat;

    JsonArray headers = doc.createNestedArray("headers");
    JsonArray values  = doc.createNestedArray("values");
    JsonArray fieldArr = doc.createNestedArray("fields");

    for (auto &pf : fields) {
        if (!statKeyLooksPlausibleApi(pf.name)) continue;
        headers.add(pf.name);
        values.add(pf.raw);
        if (!config.battery.fieldsStat.count(pf.name)) continue;
        const FieldConfig &f = config.battery.fieldsStat.at(pf.name);
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

    doc["cacheTimestamp"] = fields.empty() ? 0 : (uint32_t)millis();

    if (!fields.empty()) {
        doc["statusText"] = "STAT Live-Daten konsistent, Cache aktualisiert";
        doc["dataSource"] = "uart";
    } else {
        unsigned long now = millis();
        if (now - lastAutoStatTrigger > 6000) {
            lastAutoStatTrigger = now;
            py_scheduler.enqueue("stat 1");
            doc["statusText"] = "STAT wird gerade per UART abgefragt (Modul 1)";
        } else {
            doc["statusText"] = "Warte auf naechsten STAT-Abfrageversuch (Modul 1)";
        }
        doc["dataSource"] = "none";
    }

    if (!fields.empty()) {
        String cacheOut;
        serializeJson(doc, cacheOut);
        apiCacheSave("stat_vals", cacheOut);
    }

    server.setContentLength(measureJson(doc));
    server.send(200, "application/json", "");
    serializeJson(doc, server.client());
}

static void handleApiStatSet() {

    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    const size_t bodyLen = server.arg("plain").length();
    if (bodyLen > 120000) {
        server.send(413, "text/plain", "Payload too large");
        return;
    }

    SpiRamJsonDocument req(24576);
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Invalid JSON or payload too large for parser");
        return;
    }

    // CONFIG
    config.battery.intervalStat = req["config"]["intervalStat"] | config.battery.intervalStat;
    config.battery.enableStat   = req["config"]["enableStat"]   | config.battery.enableStat;

    // MQTT
    config.mqtt.topicStat = req["mqtt"]["topicStat"] | config.mqtt.topicStat;

    // FIELDS
    JsonArray arr = req["fields"];
    for (JsonObject f : arr) {

        String name = f["name"] | "";
        if (name.length() == 0) continue;

        if (!config.battery.fieldsStat.count(name)) {
            FieldConfig fc;
            fc.label   = name;
            fc.display = name;
            fc.factor  = "1";
            fc.unit    = "";
            fc.mqtt    = false;
            fc.send    = false;
            config.battery.fieldsStat[name] = fc;
        }

        FieldConfig &fc = config.battery.fieldsStat[name];

        fc.display = f["display"]     | fc.display;
        fc.label   = f["display"]     | fc.label;
        fc.factor  = f["factor"]      | fc.factor;
        fc.unit    = f["unit"]        | fc.unit;
        fc.mqtt    = f["sendMQTT"]    | false;
        fc.send    = f["sendPayload"] | false;
    }


	discoveryStatNeeded = true;

    // Nur die relevanten Abschnitte speichern
    config.saveSystemConfig();   // MQTT topicStat
    config.saveStatConfig();     // intervalStat, enableStat
    config.saveStatFields();     // fieldsStat
    apiCacheClear("stat_vals");

    server.send(200, "text/plain", "STAT settings saved");
}

