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
        // Build the response straight from the shared health state into a
        // PSRAM-backed JSON document while holding the mutex. This avoids a
        // full deep copy of HealthStatus on the internal heap and, crucially,
        // never reads `health` without the lock (the parser task may be
        // rewriting its vectors/strings concurrently -> use-after-free/crash).
        HealthSpiRamJsonDocument doc(8192);

        bool haveData = false;
        if (g_healthMutex && xSemaphoreTake(g_healthMutex, pdMS_TO_TICKS(300)) == pdTRUE) {

            // MODULES
            JsonArray mods = doc.createNestedArray("modules");
            for (auto &m : health.modules) {
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
            st["cellMin"]  = health.stackCellMin;
            st["cellMax"]  = health.stackCellMax;
            st["cellDiff"] = health.stackCellDiff;

            // LIST HELPERS
            auto addList = [&](JsonArray arr, const std::vector<int> &v) {
                for (int x : v) arr.add(x);
            };

            addList(doc.createNestedArray("ok"),           health.okModules);
            addList(doc.createNestedArray("warn"),         health.warnModules);
            addList(doc.createNestedArray("error"),        health.errorModules);
            addList(doc.createNestedArray("warnHistory"),  health.warnHistory);
            addList(doc.createNestedArray("errorHistory"), health.errorHistory);

            // STRONGEST + COLOR
            doc["strongest"] = health.strongestMessage;
            doc["color"]     = health.color;

            xSemaphoreGive(g_healthMutex);
            haveData = true;
        }

        if (!haveData) {
            // Could not acquire the lock in time; do not race the parser task.
            return apiError(503, F("Health busy"));
        }

        // CONFIG VALUES (not guarded by the health mutex)
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
