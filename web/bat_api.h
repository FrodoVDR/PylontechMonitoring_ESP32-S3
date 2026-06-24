#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include "../wp_webserver.h"
#include "../py_parser_bat.h"
#include "../py_scheduler.h"
#include "../py_parser_pwr.h"
#include "../config.h"
#include "api_cache.h"

struct BatSpiRamAllocator {
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

using BatSpiRamJsonDocument = BasicJsonDocument<BatSpiRamAllocator>;

static void handleApiBatCells();
static void handleApiBatSet();

static void registerBatAPI() {
    server.on("/api/bat/cells", HTTP_GET,  handleApiBatCells);
    server.on("/api/bat/set",  HTTP_POST, handleApiBatSet);
    server.on("/api/bat/refresh", HTTP_POST, []() {
        apiCacheClear("bat_cells");
        py_scheduler.enqueue("bat 1");
        server.send(200, "text/plain", "BAT refresh gestartet (Modul 1)");
    });
}

static void handleApiBatCells() {
    static unsigned long lastAutoBatTrigger = 0;

    String cached = apiCacheLoad("bat_cells");
    if (cached.length() > 0) {
        bool valid = (cached.length() >= 50
            && cached.indexOf("\"headers\"") >= 0
            && cached.indexOf("\"values\"")  >= 0);
        if (valid) {
            String out = cached;
            int lastBrace = out.lastIndexOf('}');
            if (lastBrace > 0) {
                out = out.substring(0, lastBrace)
                    + ",\"statusText\":\"BAT aus NVS-Cache geladen\""
                    + ",\"dataSource\":\"cache\""
                    + "}";
            }
            server.send(200, "application/json", out);
            return;
        }
    }

    // Schnelle Kopie unter Mutex, sofort freigeben – sendContent NIE unter Mutex!
    std::vector<BatField> fields;
    if (g_batMutex && xSemaphoreTake(g_batMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        fields = lastParsedBat.fields;
        xSemaphoreGive(g_batMutex);
    }

    BatSpiRamJsonDocument doc(12288);
    JsonObject cfg = doc.createNestedObject("config");
    cfg["intervalBat"] = config.battery.intervalBat;
    cfg["enableBat"] = config.battery.enableBat;

    JsonObject mqtt = doc.createNestedObject("mqtt");
    mqtt["topicBat"] = config.mqtt.topicBat;
    mqtt["cellPrefix"] = config.mqtt.cellPrefix;

    JsonArray headers = doc.createNestedArray("headers");
    JsonArray values  = doc.createNestedArray("values");
    JsonArray fieldArr = doc.createNestedArray("fields");

    for (auto &pf : fields) {
        headers.add(pf.name);
        values.add(pf.raw);
        if (!config.battery.fieldsBat.count(pf.name)) continue;
        const FieldConfig &f = config.battery.fieldsBat.at(pf.name);
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
        doc["statusText"] = "BAT Live-Daten konsistent, Cache aktualisiert";
        doc["dataSource"] = "uart";
    } else {
        unsigned long now = millis();
        if (now - lastAutoBatTrigger > 6000) {
            lastAutoBatTrigger = now;
            py_scheduler.enqueue("bat 1");
            doc["statusText"] = "BAT wird gerade per UART abgefragt (Modul 1)";
        } else {
            doc["statusText"] = "Warte auf naechsten BAT-Abfrageversuch (Modul 1)";
        }
        doc["dataSource"] = "none";
    }

    if (!fields.empty()) {
        String cacheOut;
        serializeJson(doc, cacheOut);
        apiCacheSave("bat_cells", cacheOut);
    }

    server.setContentLength(measureJson(doc));
    server.send(200, "application/json", "");
    serializeJson(doc, server.client());
}

static void handleApiBatSet() {

    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    BatSpiRamJsonDocument req(4096);
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    // CONFIG
    config.battery.intervalBat = req["config"]["intervalBat"] | config.battery.intervalBat;
    config.battery.enableBat   = req["config"]["enableBat"]   | config.battery.enableBat;

    // MQTT
    config.mqtt.topicBat   = req["mqtt"]["topicBat"]   | config.mqtt.topicBat;
    config.mqtt.cellPrefix = req["mqtt"]["cellPrefix"] | config.mqtt.cellPrefix;

    // FIELDS
    JsonArray arr = req["fields"];
    for (JsonObject f : arr) {

        String name = f["name"] | "";
        if (name.length() == 0) continue;

        if (!config.battery.fieldsBat.count(name)) {
            FieldConfig fc;
            fc.label   = name;
            fc.display = name;
            fc.factor  = "1";
            fc.unit    = "";
            fc.mqtt    = false;
            fc.send    = false;
            config.battery.fieldsBat[name] = fc;
        }

        FieldConfig &fc = config.battery.fieldsBat[name];

        fc.display = f["display"]     | fc.display;
        fc.label   = f["display"]     | fc.label;
        fc.factor  = f["factor"]      | fc.factor;
        fc.unit    = f["unit"]        | fc.unit;
        fc.mqtt    = f["sendMQTT"]    | false;
        fc.send    = f["sendPayload"] | false;
    }

	discoveryBatNeeded  = true;

    // Nur die relevanten Abschnitte speichern
    config.saveSystemConfig();   // MQTT topicBat, cellPrefix
    config.saveBatConfig();      // intervalBat, enableBat
    config.saveBatFields();      // fieldsBat
    apiCacheClear("bat_cells");

    server.send(200, "text/plain", "BAT settings saved");
}
