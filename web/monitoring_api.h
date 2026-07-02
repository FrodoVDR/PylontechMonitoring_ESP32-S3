#pragma once

#include "api_core.h"

#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <esp_heap_caps.h>

#include "../config.h"

struct MonitoringSpiRamAllocator {
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

using MonitoringSpiRamJsonDocument = BasicJsonDocument<MonitoringSpiRamAllocator>;

struct CpuLoadSnapshot {
    bool valid = false;
    float total = -1.0f;
    float core0 = -1.0f;
    float core1 = -1.0f;
    uint32_t taskCount = 0;
};

static CpuLoadSnapshot sampleCpuLoad() {
    static uint32_t lastTotal = 0;
    static uint32_t lastIdle0 = 0;
    static uint32_t lastIdle1 = 0;
    static bool initialized = false;

    CpuLoadSnapshot out;

    UBaseType_t taskCapacity = uxTaskGetNumberOfTasks() + 8;
    if (taskCapacity < 8) taskCapacity = 8;

    bool tasksFromPsram = true;
    TaskStatus_t* tasks = (TaskStatus_t*)heap_caps_malloc(
        taskCapacity * sizeof(TaskStatus_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!tasks) {
        tasksFromPsram = false;
        tasks = (TaskStatus_t*)pvPortMalloc(taskCapacity * sizeof(TaskStatus_t));
        if (!tasks) {
            return out;
        }
    }

    uint32_t totalRuntime = 0;
    UBaseType_t taskCount = uxTaskGetSystemState(tasks, taskCapacity, &totalRuntime);
    out.taskCount = (uint32_t)taskCount;

    if (taskCount == 0 || totalRuntime == 0) {
        if (tasksFromPsram) heap_caps_free(tasks);
        else vPortFree(tasks);
        return out;
    }

    uint32_t idle0 = 0;
    uint32_t idle1 = 0;

    for (UBaseType_t i = 0; i < taskCount; ++i) {
        const char* name = tasks[i].pcTaskName;
        if (!name) continue;

        if (strncmp(name, "IDLE", 4) == 0) {
            if (tasks[i].xCoreID == 0) {
                idle0 += tasks[i].ulRunTimeCounter;
            } else if (tasks[i].xCoreID == 1) {
                idle1 += tasks[i].ulRunTimeCounter;
            }
        }
    }

    if (tasksFromPsram) heap_caps_free(tasks);
    else vPortFree(tasks);

    if (!initialized) {
        lastTotal = totalRuntime;
        lastIdle0 = idle0;
        lastIdle1 = idle1;
        initialized = true;
        return out;
    }

    uint32_t deltaTotal = totalRuntime - lastTotal;
    uint32_t deltaIdle0 = idle0 - lastIdle0;
    uint32_t deltaIdle1 = idle1 - lastIdle1;

    lastTotal = totalRuntime;
    lastIdle0 = idle0;
    lastIdle1 = idle1;

    if (deltaTotal == 0) {
        return out;
    }

    float totalIdle = (float)deltaIdle0 + (float)deltaIdle1;
    float totalLoad = 100.0f * (1.0f - (totalIdle / (float)deltaTotal));

    // Approximate per-core denominator from dual-core total runtime window.
    float perCoreDenom = (float)deltaTotal / 2.0f;
    if (perCoreDenom <= 0.0f) {
        return out;
    }

    float core0Load = 100.0f * (1.0f - ((float)deltaIdle0 / perCoreDenom));
    float core1Load = 100.0f * (1.0f - ((float)deltaIdle1 / perCoreDenom));

    if (totalLoad < 0.0f) totalLoad = 0.0f;
    if (totalLoad > 100.0f) totalLoad = 100.0f;
    if (core0Load < 0.0f) core0Load = 0.0f;
    if (core0Load > 100.0f) core0Load = 100.0f;
    if (core1Load < 0.0f) core1Load = 0.0f;
    if (core1Load > 100.0f) core1Load = 100.0f;

    out.valid = true;
    out.total = totalLoad;
    out.core0 = core0Load;
    out.core1 = core1Load;
    return out;
}

inline void registerMonitoringAPI() {
    apiGet("/api/monitoring", []() {
        MonitoringSpiRamJsonDocument doc(2560);

        JsonObject uptime = doc.createNestedObject("uptime");
        uptime["ms"] = (uint32_t)millis();
        uptime["text"] = config.uptimeString();

        CpuLoadSnapshot cpuLoad = sampleCpuLoad();
        JsonObject cpu = doc.createNestedObject("cpu");
        cpu["tasks"] = cpuLoad.taskCount;
        cpu["valid"] = cpuLoad.valid;
        cpu["load_total"] = cpuLoad.valid ? cpuLoad.total : -1;
        cpu["load_core0"] = cpuLoad.valid ? cpuLoad.core0 : -1;
        cpu["load_core1"] = cpuLoad.valid ? cpuLoad.core1 : -1;

        JsonObject memory = doc.createNestedObject("memory");
        memory["heap_free"] = ESP.getFreeHeap();
        // Internal-DRAM low-water mark. esp_get_minimum_free_heap_size() returns
        // the GLOBAL minimum across all heap regions (incl. PSRAM), which on this
        // device is ~8 MB and useless for tracking the scarce internal heap.
        // Use the INTERNAL/8BIT capability so heap_min matches heap_free's region.
        memory["heap_min"] =
            (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        memory["heap_total"] = ESP.getHeapSize();
        memory["heap_largest_block"] =
            (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        memory["psram_free"] = ESP.getFreePsram();
        memory["psram_total"] = ESP.getPsramSize();

        JsonObject reset = doc.createNestedObject("reset");
        reset["reason"] = g_resetReasonStr;
        reset["last_stage"] = g_bootCrashStage;
        reset["last_stage_name"] = g_bootCrashStageName;
        reset["last_stage_nrt"] = g_bootCrashStageNRT;
        reset["last_stage_nrt_name"] = g_bootCrashStageNRTName;
        reset["crashes"] = g_bootCrashCount;

        JsonObject spiffs = doc.createNestedObject("spiffs");
        size_t spiffsTotal = SPIFFS.totalBytes();
        size_t spiffsUsed = SPIFFS.usedBytes();
        spiffs["total"] = (uint32_t)spiffsTotal;
        spiffs["used"] = (uint32_t)spiffsUsed;
        spiffs["free"] = (uint32_t)(spiffsTotal - spiffsUsed);

        JsonObject nvs = doc.createNestedObject("nvs");
        nvs_stats_t st = {};
        esp_err_t err = nvs_get_stats("nvs", &st);
        if (err != ESP_OK) {
            err = nvs_get_stats(NULL, &st);
        }
        bool nvsOk = (err == ESP_OK);
        nvs["ok"] = nvsOk;
        if (nvsOk) {
            nvs["total"] = st.total_entries;
            nvs["used"] = st.used_entries;
            nvs["free"] = st.free_entries;
        }

        server.setContentLength(measureJson(doc));
        server.send(200, "application/json", "");
        serializeJson(doc, server.client());
    });
}
