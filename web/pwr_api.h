#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include "../wp_webserver.h"
#include "../py_parser_pwr.h"
#include "../py_scheduler.h"
#include "../config.h"
#include "api_cache.h"

struct PwrSpiRamAllocator {
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

using PwrSpiRamJsonDocument = BasicJsonDocument<PwrSpiRamAllocator>;

static void handleApiPwrBase();
static void handleApiPwrSet();

static void registerPwrAPI() {
    server.on("/api/pwr/base", HTTP_GET,  handleApiPwrBase);
    server.on("/api/pwr/set",  HTTP_POST, handleApiPwrSet);
    server.on("/api/pwr/refresh", HTTP_POST, []() {
        apiCacheClear("pwr_base");
        py_scheduler.enqueue("pwr");
        server.send(200, "text/plain", "PWR refresh queued");
    });
}

static void handleApiPwrBase() {
    String cached = apiCacheLoad("pwr_base");
    if (cached.length() > 0) {
        server.send(200, "application/json", cached);
        return;
    }

    PwrSpiRamJsonDocument doc(8192);

    JsonObject cfg = doc.createNestedObject("config");
    cfg["intervalPwr"] = config.battery.intervalPwr;
    cfg["useFahrenheit"] = config.battery.useFahrenheit;

    JsonObject mqtt = doc.createNestedObject("mqtt");
    mqtt["topicStack"] = config.mqtt.topicStack;
    mqtt["topicPwr"] = config.mqtt.topicPwr;

    JsonArray headers = doc.createNestedArray("headers");
    JsonArray values  = doc.createNestedArray("values");
    JsonArray fields  = doc.createNestedArray("fields");

    bool hasLive = !lastParserHeader.empty();
    for (size_t i = 0; i < lastParserHeader.size(); i++) {
        const String &name = lastParserHeader[i];
        String raw = (i < lastParserValues.size()) ? lastParserValues[i] : "";
        headers.add(name);
        values.add(raw);

        if (!config.battery.fieldsPwr.count(name)) continue;
        const FieldConfig &f = config.battery.fieldsPwr.at(name);
        JsonObject o = fields.createNestedObject();
        o["name"] = name;
        o["display"] = f.display;
        o["factor"] = f.factor;
        o["unit"] = f.unit;
        o["sendMQTT"] = f.mqtt;
        o["sendPayload"] = f.send;
        o["raw"] = raw;
        o["value"] = raw;
    }

    doc["cacheTimestamp"] = hasLive ? (uint32_t)millis() : 0;

    if (hasLive) {
        String cacheOut;
        serializeJson(doc, cacheOut);
        apiCacheSave("pwr_base", cacheOut);
    } else {
        py_scheduler.enqueue("pwr");
    }

    server.setContentLength(measureJson(doc));
    server.send(200, "application/json", "");
    serializeJson(doc, server.client());
}

static void handleApiPwrSet() {

    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    PwrSpiRamJsonDocument req(4096);
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "text/plain", "Invalid JSON");
        return;
    }

    // CONFIG
    config.battery.intervalPwr = req["config"]["intervalPwr"] | config.battery.intervalPwr;
    config.battery.useFahrenheit = req["config"]["useFahrenheit"] | config.battery.useFahrenheit;

    // MQTT
    config.mqtt.topicStack = req["mqtt"]["topicStack"] | config.mqtt.topicStack;
    config.mqtt.topicPwr   = req["mqtt"]["topicPwr"]   | config.mqtt.topicPwr;

    // FIELDS
    JsonArray arr = req["fields"];
    for (JsonObject f : arr) {

        String name = f["name"] | "";
        if (name.length() == 0) continue;

        if (!config.battery.fieldsPwr.count(name)) {
            FieldConfig fc;
            fc.label   = name;
            fc.display = name;
            fc.factor  = "1";
            fc.unit    = "";
            fc.mqtt    = false;
            fc.send    = false;
            config.battery.fieldsPwr[name] = fc;
        }

        FieldConfig &fc = config.battery.fieldsPwr[name];

        fc.display = f["display"]     | fc.display;
        fc.label   = f["display"]     | fc.label;
        fc.factor  = f["factor"]      | fc.factor;
        fc.unit    = f["unit"]        | fc.unit;
        fc.mqtt    = f["sendMQTT"]    | false;
        fc.send    = f["sendPayload"] | false;
    }

    discoveryPwrNeeded = true;
    config.savePwrConfig();      // intervalPwr, useFahrenheit
    config.savePwrFields();      // fieldsPwr
    config.saveSystemConfig();   // topicPwr, topicStack (MQTT topics in system namespace)
    apiCacheClear("pwr_base");

    server.send(200, "text/plain", "PWR settings saved");
}
