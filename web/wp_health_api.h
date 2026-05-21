#pragma once
#include "api_core.h"
#include "../config.h"

extern HealthStatus health;

inline void registerHealthAPI() {

    // ---------------------------------------------------------
    // GET /api/health
    // ---------------------------------------------------------
    apiGet("/api/health", []() {

        StaticJsonDocument<4096> doc;

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

        // NEW: CONFIG VALUES
        JsonObject cfg = doc.createNestedObject("config");
        cfg["cellDiffWarn"]  = config.battery.cellDiffWarn;
        cfg["cellDiffError"] = config.battery.cellDiffError;

        String out;
        serializeJson(doc, out);
        apiJson(out);
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
        health.warnHistory.clear();
        health.errorHistory.clear();
        apiText(F("OK"));
    });
}
