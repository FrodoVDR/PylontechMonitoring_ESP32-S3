#pragma once
#include "../wp_webserver.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

inline void handleCpuApi() {
    // vTaskList() works without configGENERATE_RUN_TIME_STATS
    // Format per line: Name(padded)\tState\tPrio\tStackHWM\tTaskNum\r\n
    char buf[2048];
    memset(buf, 0, sizeof(buf));
    vTaskList(buf);

    String json = "{\"tasks\":[";
    bool first = true;

    char* saveOuter;
    char* line = strtok_r(buf, "\n", &saveOuter);

    while (line != nullptr) {
        // Remove \r
        char* cr = strchr(line, '\r');
        if (cr) *cr = 0;

        if (strlen(line) < 3) {
            line = strtok_r(nullptr, "\n", &saveOuter);
            continue;
        }

        char* saveInner;
        char* tok = strtok_r(line, "\t", &saveInner);
        if (!tok) { line = strtok_r(nullptr, "\n", &saveOuter); continue; }
        String name = String(tok);
        name.trim();

        tok = strtok_r(nullptr, "\t", &saveInner);
        if (!tok) { line = strtok_r(nullptr, "\n", &saveOuter); continue; }
        char state = tok[0];

        tok = strtok_r(nullptr, "\t", &saveInner);
        if (!tok) { line = strtok_r(nullptr, "\n", &saveOuter); continue; }
        int prio = atoi(tok);

        tok = strtok_r(nullptr, "\t", &saveInner);
        if (!tok) { line = strtok_r(nullptr, "\n", &saveOuter); continue; }
        int stack = atoi(tok);

        String stateStr;
        switch (state) {
            case 'X': stateStr = "Running";   break;
            case 'R': stateStr = "Ready";     break;
            case 'B': stateStr = "Blocked";   break;
            case 'S': stateStr = "Suspended"; break;
            case 'D': stateStr = "Deleted";   break;
            default:  stateStr = String((char)state); break;
        }

        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + name + "\",\"state\":\"" + stateStr +
                "\",\"prio\":" + String(prio) + ",\"stack\":" + String(stack) + "}";

        line = strtok_r(nullptr, "\n", &saveOuter);
    }

    json += "],";
    // Internal-DRAM free (not the global free, which includes ~8MB PSRAM and is
    // misleading). Matches /api/monitoring heap_free and the heap_min below.
    json += "\"heap_free\":"  + String((uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) + ",";
    // Internal-DRAM low-water mark (not the global min, which includes PSRAM).
    json += "\"heap_min\":"   + String((uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) + ",";
    json += "\"heap_psram\":"     + String(ESP.getFreePsram())           + ",";
    json += "\"heap_psram_total\":" + String(ESP.getPsramSize());
    json += "}";

    server.send(200, "application/json", json);
}
