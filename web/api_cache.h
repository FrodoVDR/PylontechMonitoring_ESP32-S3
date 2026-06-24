#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "esp_heap_caps.h"

static const size_t API_CACHE_CHUNK_SIZE = 1200;
static const int API_CACHE_MAX_CHUNKS = 96;   // ~115KB max per cache entry
static const size_t API_CACHE_MAX_JSON = API_CACHE_CHUNK_SIZE * API_CACHE_MAX_CHUNKS;

// Skip cache writes when free internal DRAM drops below this threshold.
// Cache saving does many transient String/NVS allocations; under heap
// pressure that spike can trigger a PANIC in the network stack. Keeping
// the (stale) existing cache is preferable to crashing.
static const size_t API_CACHE_MIN_FREE_HEAP = 18000;

static void apiCacheClear(const char* key);

static String apiCacheLoad(const char* key) {
    Preferences p;
    p.begin("api_cache", true);

    String countKey = String(key) + "_count";
    int count = p.getInt(countKey.c_str(), -1);

    String json;
    if (count >= 0) {
        if (count > API_CACHE_MAX_CHUNKS) {
            // Corrupted chunk count would cause massive allocations/loops.
            p.end();
            apiCacheClear(key);
            return "";
        }
        for (int i = 0; i < count; i++) {
            String partKey = String(key) + "_" + String(i);
            String part = p.getString(partKey.c_str(), "");
            if (part.length() == 0) {
                // Broken chunk chain -> clear corrupted cache entry.
                p.end();
                apiCacheClear(key);
                return "";
            }
            json += part;
            if (json.length() > API_CACHE_MAX_JSON) {
                p.end();
                apiCacheClear(key);
                return "";
            }
        }
    } else {
        json = p.getString(key, "");
        if (json.length() > API_CACHE_MAX_JSON) {
            p.end();
            apiCacheClear(key);
            return "";
        }
    }

    p.end();
    return json;
}

static void apiCacheSave(const char* key, const String& json) {
    if (json.length() > API_CACHE_MAX_JSON) {
        // Skip oversize payloads to protect NVS and heap.
        apiCacheClear(key);
        return;
    }

    // Under low-heap conditions, skip the write entirely. The cache save
    // performs many transient allocations (String chunks + NVS putString);
    // doing this near the heap floor risks a PANIC. Keep the existing
    // (possibly stale) cache instead of crashing.
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeHeap < API_CACHE_MIN_FREE_HEAP) {
        return;
    }

    Preferences p;
    p.begin("api_cache", false);

    p.remove(key);

    String countKey = String(key) + "_count";
    int oldCount = p.getInt(countKey.c_str(), 0);
    for (int i = 0; i < oldCount; i++) {
        String partKey = String(key) + "_" + String(i);
        p.remove(partKey.c_str());
    }

    int count = 0;
    size_t len = json.length();
    while ((size_t)count * API_CACHE_CHUNK_SIZE < len) {
        size_t start = (size_t)count * API_CACHE_CHUNK_SIZE;
        size_t end = start + API_CACHE_CHUNK_SIZE;
        if (end > len) end = len;

        String part;
        part.reserve(API_CACHE_CHUNK_SIZE);
        for (size_t i = start; i < end; i++) part += json[i];

        String partKey = String(key) + "_" + String(count);
        p.putString(partKey.c_str(), part);
        count++;
        if (count > API_CACHE_MAX_CHUNKS) {
            p.end();
            apiCacheClear(key);
            return;
        }
    }

    p.putInt(countKey.c_str(), count);
    p.end();
}

static void apiCacheClear(const char* key) {
    Preferences p;
    p.begin("api_cache", false);

    p.remove(key);

    String countKey = String(key) + "_count";
    int count = p.getInt(countKey.c_str(), 0);
    for (int i = 0; i < count; i++) {
        String partKey = String(key) + "_" + String(i);
        p.remove(partKey.c_str());
    }
    p.remove(countKey.c_str());

    p.end();
}

// Call once at startup: clears all API caches when firmware version changes.
static void apiCacheInvalidateOnFirmwareChange(const String& currentFirmwareVersion) {
    Preferences p;
    p.begin("api_cache", false);
    String stored = p.getString("fw_ver", "");
    bool changed = (stored != currentFirmwareVersion);
    if (changed) {
        p.putString("fw_ver", currentFirmwareVersion);
    }
    p.end();

    if (changed) {
        apiCacheClear("pwr_base");
        apiCacheClear("bat_cells");
        apiCacheClear("stat_vals");
        apiCacheClear("info_vals");
    }
}
