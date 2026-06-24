#pragma once
#include "api_core.h"
#include "../config.h"
#include <esp_heap_caps.h>

struct HealthSpiRamAllocator {
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

using HealthSpiRamJsonDocument = BasicJsonDocument<HealthSpiRamAllocator>;

extern HealthStatus health;

inline void registerHealthAPI() {

    // ---------------------------------------------------------
    // GET /api/health
    // ---------------------------------------------------------
    apiGet("/api/health", []() {
        HealthStatus healthSnap;
        if (g_healthMutex && xSemaphoreTake(g_healthMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            healthSnap = health;
            xSemaphoreGive(g_healthMutex);
        } else {
            healthSnap = health;
        }

        HealthSpiRamJsonDocument doc(8192);

        // MODULES
        JsonArray mods = doc.createNestedArray("modules");
        for (auto &m : healthSnap.modules) {
            JsonObject o = mods.createNestedObject();
            o["index"]    = m.index;
            o["status"]   = m.status;
            o["tempMax"]  = m.tempMax;
            o["cellMin"]  = m.cellMin;
            o["cellMax"]  = m.cellMax;
            o["cellDiff"] = m.cellDiff;
        }

        // STACK
        JsonObject st = doc.createNestedObject("stack");
        st["cellMin"]  = healthSnap.stackCellMin;
        st["cellMax"]  = healthSnap.stackCellMax;
        st["cellDiff"] = healthSnap.stackCellDiff;

        // LIST HELPERS
        auto addList = [&](JsonArray arr, const std::vector<int> &v) {
            for (int x : v) arr.add(x);
        };

        addList(doc.createNestedArray("ok"),           healthSnap.okModules);
        addList(doc.createNestedArray("warn"),         healthSnap.warnModules);
        addList(doc.createNestedArray("error"),        healthSnap.errorModules);
        addList(doc.createNestedArray("warnHistory"),  healthSnap.warnHistory);
        addList(doc.createNestedArray("errorHistory"), healthSnap.errorHistory);

        // STRONGEST + COLOR
        doc["strongest"] = healthSnap.strongestMessage;
        doc["color"]     = healthSnap.color;

        // NEW: CONFIG VALUES
        JsonObject cfg = doc.createNestedObject("config");
        cfg["cellDiffWarn"]  = config.battery.cellDiffWarn;
        cfg["cellDiffError"] = config.battery.cellDiffError;

        server.setContentLength(measureJson(doc));
        server.send(200, "application/json", "");
        serializeJson(doc, server.client());
    });

    // ---------------------------------------------------------
    // POST /api/health  (update thresholds)
    // ---------------------------------------------------------
    apiPost("/api/health", []() {

        if (!server.hasArg("plain"))
            return apiError(400, F("Missing body"));

        StaticJsonDocument<512> req;
        if (deserializeJson(req, server.arg("plain")))
            return apiError(400, F("Invalid JSON"));

        // NEW: UPDATE CONFIG VALUES
        config.battery.cellDiffWarn  = req["cellDiffWarn"]  | config.battery.cellDiffWarn;
        config.battery.cellDiffError = req["cellDiffError"] | config.battery.cellDiffError;

        config.save();
        apiText(F("Health config saved"));
    });

    // ---------------------------------------------------------
    // GET /api/health/reset
    // ---------------------------------------------------------
    apiGet("/api/health/reset", []() {
        if (g_healthMutex && xSemaphoreTake(g_healthMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            health.warnHistory.clear();
            health.errorHistory.clear();
            xSemaphoreGive(g_healthMutex);
        }
        apiText(F("OK"));
    });
}
