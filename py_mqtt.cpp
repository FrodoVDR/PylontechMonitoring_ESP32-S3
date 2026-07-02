#pragma once

#include "py_log.h"
#include "config.h"
#include "py_parser_pwr.h"
#include "py_parser_bat.h"
#include "py_parser_stat.h"
#include "py_parser_info.h"
#include "py_mqtt.h"
#include "py_mqtt_hub.h"
#include <WiFi.h>
#include <map>
#include <set>
#include <cstdlib>
#include "esp_heap_caps.h"

// Heap-floor guard: when free internal DRAM drops below this, the MQTT
// loop skips its heavy publish work (deep PwrBuffer copy with std::map,
// per-module publishes). Those operations allocate C++ containers, which
// abort() on bad_alloc (firmware built without exceptions) -> PANIC. Skip
// a cycle instead of crashing; publishing resumes once heap recovers.
static const size_t MQTT_MIN_FREE_HEAP = 15000;

/* ---------------------------------------------------------------------------
   GLOBAL STATE
   ---------------------------------------------------------------------------
   This file implements the MQTT publishing and Home Assistant discovery
   system for the Pylontech Monitor. It uses a double-buffered ParsedData
   structure and a discovery state machine to ensure stable MQTT output.
--------------------------------------------------------------------------- */

// Queue from main application (Task 1 → Task 2)
extern QueueHandle_t mqttQueue;
static PyMqttHub py_mqtt_hub;

// Parser state flags
bool parserHasData        = false;
bool newParserData        = false;
bool statParserHasData    = false;
int  statParserModuleIndex = 0;
bool batParserHasData     = false;
int  batParserModuleIndex  = 0;
bool infoParserHasData    = false;
int  infoParserModuleIndex = 0;

// Discovery triggers
bool discoveryPwrNeeded   = false;
bool discoveryBatNeeded   = false;
bool discoveryStatNeeded  = false;
bool discoveryInfoNeeded  = false;

// Discovery tracking (modules already announced)
std::set<int> discoveredPwr;
std::set<int> discoveredBat;
std::set<int> discoveredStat;
std::set<int> discoveredInfo;

// Discovery state machine
enum DiscoveryPhase {
    DISC_IDLE,
    DISC_STACK,
    DISC_PWR,
    DISC_BAT,
    DISC_STAT,
    DISC_INFO,
    DISC_DONE
};

static DiscoveryPhase discoveryPhase = DISC_IDLE;

// Discovery indices
static size_t discPwrIndex   = 0;
static size_t discBatModule  = 0;
static size_t discBatCell    = 0;
static size_t discBatField   = 0;
static size_t discStatModule = 0;
static size_t discStatField  = 0;
static size_t discInfoModule = 0;

// Reconnect timers
static unsigned long lastReconnectAttempt = 0;
static unsigned long wifiConnectedSince   = 0;
static int nextStatPublishIdx = 1;
static int nextInfoPublishIdx = 1;

// Shared serialization buffer for all MQTT publishes. Avoids a per-publish
// String allocation on the scarce internal DRAM heap. Safe as a single static
// buffer because every publish runs sequentially on Task2 (MQTT loop()).
static char s_mqttPayloadBuf[1280];

// Double-buffer for parser data
//ParsedData bufferA;
//ParsedData bufferB;
//volatile bool useA = true;

// MQTT instance
PyMqtt py_mqtt;

int PyMqtt::precisionForUnit(const String& unit) {
    if (unit == "V")  return 3;
    if (unit == "A")  return 3;
    if (unit == "°C") return 1;
    if (unit == "Ah") return 2;
    if (unit == "%")  return 0;
    return 0;
}

bool PyMqtt::precisionDiffersFromDefault(const String& unit) {
    // HA default is always 0 decimal places
    int desired = precisionForUnit(unit);
    return desired != 0;
}

bool PyMqtt::publishDirect(const String& topic, const String& payload) {
    if (!enabled || !mqttClient.connected()) return false;
    return mqttClient.publish(topic.c_str(), payload.c_str());
}


/* ---------------------------------------------------------------------------
   LOGGING HELPERS
   ---------------------------------------------------------------------------
   Unified logging wrapper to ensure consistent formatting and log levels.
--------------------------------------------------------------------------- */

static void logInfo(const String& msg)  { Log(LOG_INFO,  msg); }
static void logWarn(const String& msg)  { Log(LOG_WARN,  msg); }
static void logError(const String& msg) { Log(LOG_ERROR, msg); }
static void logDebug(const String& msg) { Log(LOG_DEBUG, msg); }

static bool isTextLikeField(const FieldConfig& fc) {
    return (fc.factor == "text" || fc.factor == "date" || fc.unit == "timestamp");
}

static bool tryParseNumberStrict(const String& in, double& out) {
    String t = in;
    t.trim();
    if (t.length() == 0) return false;

    const char* s = t.c_str();
    char* endPtr = nullptr;
    out = strtod(s, &endPtr);
    if (endPtr == s || endPtr == nullptr) return false;

    while (*endPtr == ' ' || *endPtr == '\t' || *endPtr == '\r' || *endPtr == '\n') endPtr++;
    return (*endPtr == '\0');
}

static bool isIntegerLike(double v) {
    long iv = (long)v;
    return v == (double)iv;
}

// Strict BAT field validation to prevent publishing malformed values.
static bool validateBatMqttField(const String& fieldName,
                                 String& value,
                                 String& reason) {
    String key = fieldName;
    key.toLowerCase();

    if (key == "bal" || key == "balancer") {
        String b = value;
        b.trim();
        b.toUpperCase();
        if (b == "N" || b == "Y") {
            value = b;
            return true;
        }
        reason = "balancer not in {N,Y}";
        return false;
    }

    if (key == "battery" || key == "volt" || key == "curr" ||
        key == "tempr" || key == "soc" || key == "coulomb") {
        double num = 0.0;
        if (!tryParseNumberStrict(value, num)) {
            reason = "non-numeric value '" + value + "'";
            return false;
        }

        if (key == "battery") {
            if (!isIntegerLike(num) || num < 0.0 || num > 14.0) {
                reason = "battery out of range [0..14]";
                return false;
            }
            value = String((int)num);
            return true;
        }

        if (key == "volt") {
            if (num < 2.0 || num > 4.0) {
                reason = "volt out of range [2..4]";
                return false;
            }
            return true;
        }

        if (key == "curr") {
            if (num < -250.0 || num > 250.0) {
                reason = "curr out of range [-250..250]";
                return false;
            }
            return true;
        }

        if (key == "tempr") {
            if (num < -20.0 || num > 60.0) {
                reason = "tempr out of range [-20..60]";
                return false;
            }
            return true;
        }

        if (key == "soc" || key == "coulomb") {
            if (num < 0.0 || num > 100.0) {
                reason = key + " out of range [0..100]";
                return false;
            }
            return true;
        }
    }

    return true;
}

static String compactLowerKey(const String& in) {
    String k;
    k.reserve(in.length());
    for (char c : in) {
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            k += c;
        }
    }
    return k;
}

// Validate PWR MQTT fields before publish. Invalid values are skipped.
static bool validatePwrMqttField(const String& fieldName,
                                 const String& displayName,
                                 String& value,
                                 String& reason) {
    String key = compactLowerKey(fieldName);
    String disp = compactLowerKey(displayName);

    auto isAny = [&](const String& a, const String& b) {
        return (key == a || key == b || disp == a || disp == b);
    };

    bool isSoc = (key == "soc" || key == "coulomb" || disp == "soc");
    bool isCurrent = isAny("curr", "current");
    bool isMosTempr = (key == "mostempr" || key == "mostemp" || disp == "mostempr" || disp == "mostemp");
    bool isPower = (key == "power" || disp == "power");
    bool isTemperature = isAny("tempr", "temperature");
    bool isVhigh = (key == "vhigh" || disp == "vhigh");
    bool isVlow = (key == "vlow" || disp == "vlow");
    bool isVoltage = isAny("volt", "voltage");

    if (!(isSoc || isCurrent || isMosTempr || isPower || isTemperature || isVhigh || isVlow || isVoltage)) {
        return true;
    }

    double num = 0.0;
    if (!tryParseNumberStrict(value, num)) {
        reason = "non-numeric value '" + value + "'";
        return false;
    }

    if (isSoc) {
        if (num < 0.0 || num > 100.0) {
            reason = "SOC out of range [0..100]";
            return false;
        }
        return true;
    }

    if (isCurrent) {
        if (num < -250.0 || num > 250.0) {
            reason = "Current out of range [-250..250]";
            return false;
        }
        return true;
    }

    if (isMosTempr || isTemperature) {
        if (num < -20.0 || num > 60.0) {
            reason = "Temperature out of range [-20..60]";
            return false;
        }
        return true;
    }

    if (isVhigh || isVlow) {
        if (num < 2.0 || num > 4.5) {
            reason = (isVhigh ? "Vhigh" : "Vlow") + String(" out of range [2..4.5]");
            return false;
        }
        return true;
    }

    // Power and Voltage must be numeric only.
    if (isPower || isVoltage) {
        return true;
    }

    return true;
}

static void runMqttValidationSelfTestOnce() {
    static bool alreadyRun = false;
    if (alreadyRun) return;
    alreadyRun = true;

    int failures = 0;

    struct BatCase {
        const char* field;
        const char* value;
        bool expectOk;
    };
    const BatCase batCases[] = {
        {"Volt", "3.347", true},
        {"Volt", "334essfully", false},
        {"Curr", "-21.3", true},
        {"Curr", "-251", false},
        {"Tempr", "29.4", true},
        {"Tempr", "120", false},
        {"SOC", "56", true},
        {"SOC", "101", false},
        {"BAL", "N", true},
        {"BAL", "X", false}
    };

    for (const auto& tc : batCases) {
        String value = tc.value;
        String reason;
        bool ok = validateBatMqttField(tc.field, value, reason);
        if (ok != tc.expectOk) {
            failures++;
            logError("MQTT self-test BAT failed: field='" + String(tc.field) +
                     "' value='" + String(tc.value) +
                     "' expected=" + String(tc.expectOk ? "ok" : "reject") +
                     " got=" + String(ok ? "ok" : "reject"));
        }
    }

    struct PwrCase {
        const char* field;
        const char* display;
        const char* value;
        bool expectOk;
    };
    const PwrCase pwrCases[] = {
        {"SOC", "SOC", "88", true},
        {"SOC", "SOC", "188", false},
        {"Curr", "Current", "-220", true},
        {"Curr", "Current", "-320", false},
        {"MosTempr", "MosTempr", "45", true},
        {"MosTempr", "MosTempr", "80", false},
        {"Vhigh", "Vhigh", "3.55", true},
        {"Vhigh", "Vhigh", "5.20", false},
        {"Voltage", "Voltage", "50.12", true},
        {"Voltage", "Voltage", "bad-data", false}
    };

    for (const auto& tc : pwrCases) {
        String value = tc.value;
        String reason;
        bool ok = validatePwrMqttField(tc.field, tc.display, value, reason);
        if (ok != tc.expectOk) {
            failures++;
            logError("MQTT self-test PWR failed: field='" + String(tc.field) +
                     "' value='" + String(tc.value) +
                     "' expected=" + String(tc.expectOk ? "ok" : "reject") +
                     " got=" + String(ok ? "ok" : "reject"));
        }
    }

    if (failures == 0) {
        logInfo("MQTT self-test: BAT/PWR validation passed");
    } else {
        logError("MQTT self-test: BAT/PWR validation failures=" + String(failures));
    }
}

static void setTypedJsonValue(JsonDocument& doc,
                              const String& key,
                              const String& value,
                              const FieldConfig& fc) {
    if (isTextLikeField(fc)) {
        doc[key] = value;
        return;
    }

    double number = 0.0;
    if (tryParseNumberStrict(value, number)) {
        doc[key] = number;
    } else {
        doc[key] = value;
    }
}

/* ---------------------------------------------------------------------------
   DISCOVERY RESET
   ---------------------------------------------------------------------------
   Resets the discovery state machine and clears module tracking.
--------------------------------------------------------------------------- */
void PyMqtt::resetDiscovery(bool pwr, bool bat, bool stat) {

    if (pwr) discoveredPwr.clear();
    if (bat) discoveredBat.clear();
    if (stat) discoveredStat.clear();
    discoveredInfo.clear();

    discoveryPhase = DISC_STACK;
    discPwrIndex   = 0;
    discBatModule  = 0;
    discBatCell    = 0;
    discBatField   = 0;
    discStatModule = 0;
    discStatField  = 0;
    discInfoModule = 0;

    discoveryActive = true;

    logInfo("MQTT: Discovery reset triggered");
}

/* ---------------------------------------------------------------------------
   BEGIN
   ---------------------------------------------------------------------------
   Initializes MQTT client and marks discovery as required.
--------------------------------------------------------------------------- */
void PyMqtt::begin() {
    enabled = config.mqtt.enabled;

    // One-shot self-test for strict BAT/PWR validation rules.
    runMqttValidationSelfTestOnce();

    if (!enabled) {
        logInfo("MQTT disabled in configuration");
        return;
    }

    mqttClient.setServer(config.mqtt.server.c_str(), config.mqtt.port);
    mqttClient.setBufferSize(2048);

    // Discovery darf NICHT automatisch starten
    discoveryPhase = DISC_IDLE;
    discoveryActive = false;

    logInfo("MQTT: BufferSize set to 2048 bytes");
}


/* ---------------------------------------------------------------------------
   CONNECT
   ---------------------------------------------------------------------------
   Attempts to connect to the MQTT broker. Connection is delayed until WiFi
   has been stable for at least 10 seconds to avoid rapid reconnect loops.
--------------------------------------------------------------------------- */
bool PyMqtt::connect() {
    if (!enabled) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    if (wifiConnectedSince == 0)
        wifiConnectedSince = millis();

    // Wait 10 seconds after WiFi connects
    if (millis() - wifiConnectedSince < 10000)
        return false;

    String clientId = "PylontechMonitor-" + String((uint32_t)ESP.getEfuseMac());

    bool ok = mqttClient.connect(
        clientId.c_str(),
        config.mqtt.user.c_str(),
        config.mqtt.pass.c_str()
    );

    if (ok)
        logInfo("MQTT connected as " + clientId);
    else
        logWarn("MQTT connection failed");

    return ok;
}

/* ---------------------------------------------------------------------------
   RAW PUBLISH (Task 1 → Task 2)
   ---------------------------------------------------------------------------
   Publishes raw messages from the queue. Used for low-level or custom topics.
--------------------------------------------------------------------------- */
bool PyMqtt::publishRaw(const String& topic, const String& payload) {
    if (!enabled || !mqttClient.connected()) return false;
    return mqttClient.publish(topic.c_str(), payload.c_str());
}

/* ---------------------------------------------------------------------------
   LOOP (Task 2)
   ---------------------------------------------------------------------------
   Main MQTT loop:
   - Handles reconnects
   - Processes queue messages
   - Publishes PWR/BAT/STAT data
   - Runs discovery state machine
--------------------------------------------------------------------------- */
void PyMqtt::loop() {
    if (!enabled) return;

    // Heap-floor guard (see MQTT_MIN_FREE_HEAP). Under critically low DRAM,
    // skip the heavy publish work this cycle to avoid a C++ container
    // bad_alloc -> abort() -> PANIC. Keep the MQTT connection serviced so
    // it stays alive; data resumes once heap recovers.
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeHeap < MQTT_MIN_FREE_HEAP) {
        static unsigned long lastLowHeapLog = 0;
        if (millis() - lastLowHeapLog > 10000) {
            lastLowHeapLog = millis();
            logWarn("MQTT: low heap (" + String((uint32_t)freeHeap) +
                    "B) -> skipping publish cycle");
        }
        if (mqttClient.connected()) mqttClient.loop();
        return;
    }

    // PWR is published directly under g_pwrMutex further down (read-in-place, no
    // deep copy). Copying the whole PwrBuffer here duplicated all 8 modules'
    // FieldMap Strings every cycle -> a large transient internal-DRAM spike that,
    // coinciding with other allocations, drove the heap to the floor and caused
    // OOM PANICs (observed at nrt:web_loop / nrt:sys_loop, heap_min a few hundred B).
    BatBuffer* bat  = batUseA ? &batA : &batB;
    StatBuffer* stat = statUseA ? &statA : &statB;

    // WiFi check
    if (WiFi.status() != WL_CONNECTED) {
        wifiConnectedSince = 0;
        return;
    }

    // MQTT reconnect
    if (!mqttClient.connected()) {
        if (millis() - lastReconnectAttempt > 3000) {
            lastReconnectAttempt = millis();
            connect();
        }
        return;
    }

    // ---------------------------------------------------------
    // DISCOVERY TRIGGER (NEU: startet IMMER, wenn Flag gesetzt)
    // ---------------------------------------------------------
    if (discoveryPwrNeeded || discoveryBatNeeded || discoveryStatNeeded) {

        logInfo("MQTT: Discovery start requested");

        discoveryPhase = DISC_STACK;
        discoveryActive = true;

        discoveryPwrNeeded  = false;
        discoveryBatNeeded  = false;
        discoveryStatNeeded = false;
        discoveryInfoNeeded = false;
    }

    // ---------------------------------------------------------
    // RAW QUEUE
    // ---------------------------------------------------------
    MqttMessage msg;
    while (xQueueReceive(mqttQueue, &msg, 0) == pdTRUE) {
        publishRaw(String(msg.topic), String(msg.payload));
    }

    // ---------------------------------------------------------
    // PUBLISH PWR (Stack mode vs Hub mode)
    // ---------------------------------------------------------

    // ---------------------------------------------------------
    // PUBLISH PWR (Stack mode vs Hub mode)
    // Read the active buffer IN PLACE under g_pwrMutex - no deep PwrBuffer copy.
    // publishStack/publishBat serialize into a static buffer and write to the
    // PubSubClient buffer, so the critical section stays short. The RT parser's
    // buffer-swap takes the same mutex with a 50ms timeout and simply retries on
    // the rare contention, so holding it briefly here is safe.
    // ---------------------------------------------------------
    if (g_pwrMutex && xSemaphoreTake(g_pwrMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (parserHasData) {
            parserHasData = false;
            const PwrBuffer& pwrActive = pwrUseA ? pwrA : pwrB;

            if (g_batteryMode == BatteryMode::STACK) {
                publishStack(pwrActive.stack);

                for (const auto& mod : pwrActive.modules) {
                    if (!mod.present) continue;
                    publishBat(mod.index, mod);
                }
            }
            else if (g_batteryMode == BatteryMode::HUB) {
                py_mqtt_hub.publishHubStacks(lastParsedHub);
                py_mqtt_hub.publishHubModules(pwrActive.modules);
            }
        }
        xSemaphoreGive(g_pwrMutex);
    }

    // ---------------------------------------------------------
    // PUBLISH BAT CELLS (single double-buffer; contamination fixes in the
    // parser ensure every module parses reliably, so the latest parsed module
    // is published between BAT parses without per-module buffering. This keeps
    // internal DRAM usage bounded (2 buffers instead of one per module).
    // ---------------------------------------------------------
    if (batParserHasData) {
        int moduleIndex = batParserModuleIndex;
        if (g_batMutex && xSemaphoreTake(g_batMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            const BatBuffer& active = batUseA ? batA : batB;
            if (!active.cells.empty()) {
                publishBatCells(moduleIndex, active.cells);
            }
            xSemaphoreGive(g_batMutex);
        }
        batParserHasData = false;
    }

    // ---------------------------------------------------------
    // PUBLISH STAT (gedrosselt: max. 1 pending Modul pro loop)
    // ---------------------------------------------------------
    if (g_statMutex && xSemaphoreTake(g_statMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        int checked = 0;
        int idx = nextStatPublishIdx;
        while (checked < (MAX_BATTERY_MODULES - 1)) {
            if (idx <= 0 || idx >= MAX_BATTERY_MODULES) idx = 1;
            if (g_pendingStatReady[idx]) {
                g_pendingStatReady[idx] = false;
                publishStat(idx, g_pendingStat[idx]);
                idx++;
                break;
            }
            idx++;
            checked++;
        }
        if (idx >= MAX_BATTERY_MODULES) idx = 1;
        nextStatPublishIdx = idx;
        xSemaphoreGive(g_statMutex);
    }
    statParserHasData = false;  // legacy flag cleared

    // ---------------------------------------------------------
    // PUBLISH INFO (gedrosselt: max. 1 pending Modul pro loop)
    // ---------------------------------------------------------
    if (g_infoMutex && xSemaphoreTake(g_infoMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        int checked = 0;
        int idx = nextInfoPublishIdx;
        while (checked < (MAX_BATTERY_MODULES - 1)) {
            if (idx <= 0 || idx >= MAX_BATTERY_MODULES) idx = 1;
            if (g_pendingInfoReady[idx]) {
                g_pendingInfoReady[idx] = false;
                publishInfo(idx, g_pendingInfo[idx]);
                idx++;
                break;
            }
            idx++;
            checked++;
        }
        if (idx >= MAX_BATTERY_MODULES) idx = 1;
        nextInfoPublishIdx = idx;
        xSemaphoreGive(g_infoMutex);
    }
    infoParserHasData = false;  // legacy flag cleared

    // ---------------------------------------------------------
    // DISCOVERY STATE MACHINE
    // ---------------------------------------------------------
    // Discovery spans multiple loop iterations (one phase per call). Use the
    // latest known PWR buffer for discovery so module presence/indices are always
    // available, even on iterations without fresh PWR data (lock-free
    // latest-buffer pattern, same as bat/stat above).
    PwrBuffer* pwrLatest = pwrUseA ? &pwrA : &pwrB;
    handleDiscoveryStep(*pwrLatest, *bat, *stat);

    mqttClient.loop();
}


/* ---------------------------------------------------------------------------
   DISCOVERY STATE MACHINE (FRAMEWORK ONLY)
   ---------------------------------------------------------------------------
   The detailed discovery functions (publishDiscoveryStack, publishDiscoveryPwr,
   publishDiscoveryBatField, publishDiscoveryStatField) will be implemented in
   PART 3.
--------------------------------------------------------------------------- */
void PyMqtt::handleDiscoveryStep(
    const PwrBuffer& pwr,
    const BatBuffer& bat,
    const StatBuffer& stat
) {
    if (!enabled || !mqttClient.connected()) return;

    // Heap-floor guard: discovery builds many JsonDocuments + String payloads in
    // a short window. Under critically low internal DRAM this is what pushed
    // heap_min to ~8KB and caused PANIC stage 130 (rt:wait_queue). Defer the
    // step (retry next loop) instead of allocating into the danger zone. Normal
    // free internal heap is ~57KB, so this only trips during transient spikes.
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < MQTT_MIN_FREE_HEAP) {
        return;
    }

    auto stableModuleCount = [&]() -> size_t {
        size_t count = 0;
        if (pwr.stack.batteryCount > 0) count = (size_t)pwr.stack.batteryCount;
        if (config.detectedModules > count) count = config.detectedModules;
        if (config.battery.maxModules > 0 && count > config.battery.maxModules) {
            count = config.battery.maxModules;
        }
        return count;
    };

    // ---------------------------------------------------------
    // HUB DISCOVERY OVERRIDE
    // ---------------------------------------------------------
    if (g_batteryMode == BatteryMode::HUB) {
        // Run Hub discovery once
        py_mqtt_hub.publishHubDiscovery(lastParsedHub);

        // Mark discovery as finished
        discoveryPhase = DISC_DONE;
        discoveryActive = false;

        return;
    }


    switch (discoveryPhase) {

        case DISC_IDLE:
        case DISC_DONE:
            return;

        case DISC_STACK:
            publishDiscoveryStack();
            discoveryPhase = DISC_PWR;
            discPwrIndex = 0;
            return;

        case DISC_PWR:
            if (discPwrIndex >= pwr.modules.size()) {
                discoveryPhase = DISC_BAT;
                discBatModule = 0;
                discBatCell   = 0;
                return;
            }
            if (!pwr.modules[discPwrIndex].present) {
                discPwrIndex++;
                return;
            }
            publishDiscoveryPwrModule(pwr.modules[discPwrIndex].index);
            discPwrIndex++;
            return;

        case DISC_BAT:
            if (discBatModule >= pwr.modules.size()) {
                discoveryPhase = DISC_STAT;
                discStatModule = 0;
                return;
            }
            if (!pwr.modules[discBatModule].present) {
                discBatModule++;
                discBatCell = 0;
                return;
            }
            // Publish ONE cell per loop iteration. Previously all 15 cells of a
            // module (each with several fields) were published in one tight
            // loop -> a burst of dozens of JsonDocument+String allocations that
            // spiked internal DRAM (heap_min ~8KB) and pegged core0, causing
            // PANIC stage 130 (rt:wait_queue). Spreading the work across
            // iterations keeps the heap floor stable.
            if (discBatCell < bat.cells.size()) {
                publishDiscoveryBatCell(pwr.modules[discBatModule].index,
                                        (int)discBatCell);
                discBatCell++;
                return;
            }
            discBatCell = 0;
            discBatModule++;
            return;

        case DISC_STAT:
            if (discStatModule >= stableModuleCount()) {
                discoveryPhase = DISC_INFO;
                discInfoModule = 0;
                return;
            }
            publishDiscoveryStatModule((int)discStatModule + 1);
            discStatModule++;
            return;

        case DISC_INFO:
            if (discInfoModule >= stableModuleCount()) {
                discoveryPhase = DISC_DONE;
                return;
            }
            publishDiscoveryInfoModule((int)discInfoModule + 1);
            discInfoModule++;
            return;
    }
}

/* ---------------------------------------------------------------------------
   NAME NORMALIZATION
   ---------------------------------------------------------------------------
   Converts display names into safe MQTT/JSON identifiers:
   - Removes spaces and special characters
   - Converts to CamelCase
   - Ensures Home Assistant compatibility
--------------------------------------------------------------------------- */
String PyMqtt::normalizeName(const String& in) {
    String out;
    bool upperNext = true;

    for (char c : in) {
        if (c == ' ' || c == '_' || c == '-' || c == '.') {
            upperNext = true;
            continue;
        }
        if (!isalnum(c)) continue;

        if (upperNext) {
            out += (char)toupper(c);
            upperNext = false;
        } else {
            out += (char)c;
        }
    }
    return out;
}

/* ---------------------------------------------------------------------------
   ID SANITIZER
   ---------------------------------------------------------------------------
   Builds Home Assistant safe identifiers:
   - lowercase
   - only [a-z0-9_]
   - spaces and separators → underscore
--------------------------------------------------------------------------- */
static String sanitizeId(const String& in) {
    String out;
    out.reserve(in.length());
    for (char c : in) {
        if (c >= 'A' && c <= 'Z') {
            out += char(c + 32); // tolower
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if (c == '_' || c == '-' || c == ' ' || c == '/' || c == '.') {
            out += '_';
        } else {
            // ignore other characters
        }
    }
    return out;
}

/* ---------------------------------------------------------------------------
   DECIMAL PRECISION BASED ON UNIT
--------------------------------------------------------------------------- */
int PyMqtt::decimalsForUnit(const String& unit) {
    if (unit == "V")  return 3;   // voltage
    if (unit == "A")  return 3;   // current
    if (unit == "°C") return 1;   // temperature
    if (unit == "%")  return 0;   // percentage
    if (unit == "Ah") return 3;   // capacity
    return 0;                     // default
}

/* ---------------------------------------------------------------------------
   DEVICE CLASS BASED ON UNIT
--------------------------------------------------------------------------- */
String PyMqtt::deviceClassForUnit(const String& unit) {
    if (unit == "V")  return "voltage";
    if (unit == "A")  return "current";
    if (unit == "°C") return "temperature";
    if (unit == "%")  return "battery";
    return "";
}

/* ---------------------------------------------------------------------------
   COMPUTE VALUE (NUMERIC OR TEXT)
--------------------------------------------------------------------------- */
String PyMqtt::computeValue(const String& raw, const FieldConfig& fc) {

    // Time fields → always text
    if (fc.factor == "date" || fc.unit == "timestamp")
        return raw;

    // Text fields → unchanged
    if (fc.factor == "text")
        return raw;

    // If raw is not numeric, keep original text instead of publishing 0.
    // This avoids broken downstream evaluations for INFO text fields.
    const char* s = raw.c_str();
    char* endPtr = nullptr;
    strtof(s, &endPtr);
    if (endPtr == s || (endPtr && *endPtr != '\0')) {
        return raw;
    }

    // Numeric conversion
    float factor = fc.factor.toFloat();
    float valueC = raw.toFloat() * factor;

    // Time fields → convert "YYYY-MM-DD HH:MM:SS" → "YYYY-MM-DDTHH:MM:SS"
    if (fc.factor == "date" || fc.unit == "timestamp") {
        String iso = raw;
        iso.replace(" ", "T");
        return iso;
    }


    // Fahrenheit conversion if enabled
    if (fc.unit == "°C" && config.battery.useFahrenheit) {
        float valueF = valueC * 1.8f + 32.0f;
        return String(valueF, decimalsForUnit("°F"));
    }

    return String(valueC, decimalsForUnit(fc.unit));
}

/* ---------------------------------------------------------------------------
   BUILD MQTT TOPIC
--------------------------------------------------------------------------- */
String PyMqtt::buildTopic(
    const String& subtopic,
    int moduleIndex,
    const String& fieldName,
    int cellIndex,
    bool isCell
) {
    String prefix = config.mqtt.prefix;
    String cellPrefix = config.mqtt.cellPrefix; // e.g. "Cell"

    if (isCell) {
        return prefix + "/" + subtopic + "/" +
               String(moduleIndex) + "/" +
               cellPrefix + String(cellIndex) + "_" + fieldName;
    }

    return prefix + "/" + subtopic + "/" +
           String(moduleIndex) + "/" + fieldName;
}

String getStackHealthState() {
    if (!health.errorModules.empty()) return "ERROR";
    if (!health.warnModules.empty())  return "WARN";
    return "OK";
}

String getModuleStatusString() {
    String out = "";
    for (auto &m : health.modules) {
        if (m.status == "Fehler")       out += "2";
        else if (m.status == "Warnung") out += "1";
        else                            out += "0";
    }
    return out;
}


/* ---------------------------------------------------------------------------
   PUBLISH STACK JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishStack(const BatteryStack& stack) {
    if (!enabled || !mqttClient.connected()) return;

    String topic = config.mqtt.prefix + "/" + config.mqtt.topicStack;
    float stackVolt = stack.avgVoltage_mV / 1000.0f;
    float stackCurr = stack.totalCurrent_mA / 1000.0f;
    float stackPower = stackCurr * stackVolt;

    StaticJsonDocument<256> doc;
    doc["StackVoltAvg"] = stackVolt;
    doc["StackCurrSum"] = stackCurr;
    doc["StackPower"] = stackPower;
    doc["StackPowerIn"] = (stackPower > 0.0f) ? stackPower : 0.0f;
    doc["StackPowerOut"] = (stackPower < 0.0f) ? stackPower : 0.0f;
    doc["StackTempMax"] = stack.temperature / 1000.0f;
    doc["BatteryCount"] = stack.batteryCount;
    doc["SOC"] = stack.soc;

    // Health-State
    doc["HealthState"] = getStackHealthState();

    // Kompakter Modulstatus
    doc["ModuleState"] = getModuleStatusString();

    serializeJson(doc, s_mqttPayloadBuf, sizeof(s_mqttPayloadBuf));

    static unsigned long lastStackDebugLog = 0;
    unsigned long now = millis();
    if (now - lastStackDebugLog > 30000) {
        logDebug("MQTT stack publish: SOC=" + String(stack.soc) +
                 " BatteryCount=" + String(stack.batteryCount));
        lastStackDebugLog = now;
    }

    mqttClient.publish(topic.c_str(), s_mqttPayloadBuf);
}


/* ---------------------------------------------------------------------------
   PUBLISH PWR MODULE JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishBat(int index, const BatteryModule& mod) {
    if (!enabled || !mqttClient.connected() || !mod.present)
        return;

    String subtopic = config.mqtt.topicPwr;
    String topic = config.mqtt.prefix + "/" + subtopic + "/" + String(index);

    StaticJsonDocument<512> doc;

    bool anyMqttFieldEnabled = false;
    bool anyFieldMatched = false;
    int invalidPwrFieldCount = 0;

    for (auto &kv : config.battery.fieldsPwr) {
        const String& fieldName = kv.first;
        const FieldConfig& fc = kv.second;

        if (!fc.mqtt) continue;
        anyMqttFieldEnabled = true;

        auto it = mod.fields.find(fieldName);
        if (it == mod.fields.end()) continue;
        anyFieldMatched = true;

        String display = normalizeName(fc.display);
        String value = computeValue(it->second, fc);

        String pwrReason;
        if (!validatePwrMqttField(fieldName, fc.display, value, pwrReason)) {
            invalidPwrFieldCount++;
            static unsigned long lastInvalidPwrLog = 0;
            unsigned long now = millis();
            if (now - lastInvalidPwrLog > 3000) {
                logWarn("MQTT PWR skipped invalid field: module=" + String(index) +
                        " field='" + fieldName + "' reason=" + pwrReason);
                lastInvalidPwrLog = now;
            }
            continue;
        }

        // Some setups keep the module index field as text in UI (Power/Battery),
        // but MQTT consumers expect numeric JSON.
        bool isPowerIndexField = (fieldName == "Power" ||
                                  fieldName == "Battery" ||
                                  display == "Power");
        if (isPowerIndexField) {
            double number = 0.0;
            if (tryParseNumberStrict(value, number)) {
                doc[display] = number;
            } else {
                doc[display] = value;
            }
        } else {
            setTypedJsonValue(doc, display, value, fc);
        }

    }

    config.lastMqttContact = config.getCurrentTimeString();

    if (doc.size() == 0) {
        static unsigned long lastNoPwrMqttLog = 0;
        unsigned long now = millis();
        if (now - lastNoPwrMqttLog > 30000) {
            if (!anyMqttFieldEnabled) {
                logWarn("MQTT PWR skipped: no MQTT fields enabled in fieldsPwr");
            } else if (!anyFieldMatched) {
                logWarn("MQTT PWR skipped: configured field names do not match parsed PWR keys");
            } else if (invalidPwrFieldCount > 0) {
                logWarn("MQTT PWR skipped: payload empty after strict validation");
            } else {
                logWarn("MQTT PWR skipped: payload empty after value conversion");
            }
            lastNoPwrMqttLog = now;
        }
        return;
    }

    serializeJson(doc, s_mqttPayloadBuf, sizeof(s_mqttPayloadBuf));

    if (!mqttClient.publish(topic.c_str(), s_mqttPayloadBuf))
        logWarn("MQTT publish failed: " + topic);
}

/* ---------------------------------------------------------------------------
   PUBLISH BAT CELLS JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishBatCells(int moduleIndex, const std::vector<BatData>& batCells) {
    if (!enabled || !mqttClient.connected()) return;
    if (batCells.empty()) return;

    String subtopic = config.mqtt.topicBat;

    bool anyMqttFieldEnabled = false;
    for (auto &kv : config.battery.fieldsBat) {
        if (kv.second.mqtt) {
            anyMqttFieldEnabled = true;
            break;
        }
    }
    if (!anyMqttFieldEnabled) {
        static unsigned long lastNoBatMqttLog = 0;
        unsigned long now = millis();
        if (now - lastNoBatMqttLog > 30000) {
            logWarn("MQTT BAT skipped: no MQTT fields enabled in fieldsBat");
            lastNoBatMqttLog = now;
        }
        return;
    }

    bool publishedAnyCell = false;
    int invalidCellCount = 0;

    for (const auto& cell : batCells) {

        StaticJsonDocument<512> doc;
        bool cellValid = true;
        String invalidReason;

        for (auto &f : cell.fields) {

            // Feld existiert in der BAT-Konfiguration?
            if (!config.battery.fieldsBat.count(f.name)) continue;
            const FieldConfig &fc = config.battery.fieldsBat[f.name];
            if (!fc.mqtt) continue;

            // WICHTIG:
            // Discovery benutzt fc.display als JSON-Key → Publisher muss das auch tun
            String key   = fc.display;
            String value = computeValue(f.raw, fc);

            if (!validateBatMqttField(f.name, value, invalidReason)) {
                cellValid = false;
                invalidReason = "field '" + f.name + "': " + invalidReason;
                break;
            }

            setTypedJsonValue(doc, key, value, fc);
        }

        if (!cellValid) {
            invalidCellCount++;
            static unsigned long lastInvalidBatLog = 0;
            unsigned long now = millis();
            if (now - lastInvalidBatLog > 3000) {
                logWarn("MQTT BAT skipped invalid cell: module=" + String(moduleIndex) +
                        " cell=" + String(cell.cellIndex) + " " + invalidReason);
                lastInvalidBatLog = now;
            }
            continue;
        }

        if (doc.size() == 0) continue;  // keine MQTT-Felder konfiguriert

        // Add module number (1..N) to every published BAT cell payload.
        if (moduleIndex < 1 || moduleIndex > 14) {
            invalidCellCount++;
            static unsigned long lastInvalidNumberLog = 0;
            unsigned long now = millis();
            if (now - lastInvalidNumberLog > 3000) {
                logWarn("MQTT BAT skipped: module Number out of range [1..14], got " + String(moduleIndex));
                lastInvalidNumberLog = now;
            }
            continue;
        }
        doc["Number"] = moduleIndex;

        serializeJson(doc, s_mqttPayloadBuf, sizeof(s_mqttPayloadBuf));

        String topic =
            config.mqtt.prefix + "/" + subtopic + "/" +
            String(moduleIndex) + "/" +
            config.mqtt.cellPrefix + String(cell.cellIndex);

        if (mqttClient.publish(topic.c_str(), s_mqttPayloadBuf)) {
            publishedAnyCell = true;
        }
    }

    if (!publishedAnyCell) {
        static unsigned long lastNoBatMatchLog = 0;
        unsigned long now = millis();
        if (now - lastNoBatMatchLog > 30000) {
            if (invalidCellCount > 0) {
                logWarn("MQTT BAT skipped: all cells rejected by strict validation");
            } else {
                logWarn("MQTT BAT skipped: configured field names do not match parsed BAT keys");
            }
            lastNoBatMatchLog = now;
        }
    }
}

/* ---------------------------------------------------------------------------
   PUBLISH STAT JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishStat(int moduleIndex, const StatData& stat) {
    if (!enabled || !mqttClient.connected()) return;
    if (stat.fields.empty()) return;

    String subtopic = config.mqtt.topicStat;
    String topic = config.mqtt.prefix + "/" + subtopic + "/" + String(moduleIndex);

    StaticJsonDocument<1024> doc;

    for (auto &f : stat.fields) {

        if (!config.battery.fieldsStat.count(f.name)) continue;
        const FieldConfig &fc = config.battery.fieldsStat[f.name];
        if (!fc.mqtt) continue;

        String display = normalizeName(fc.display);
        String value = computeValue(f.raw, fc);

        setTypedJsonValue(doc, display, value, fc);

    }

    if (doc.size() == 0) return;  // keine MQTT-Felder konfiguriert

    serializeJson(doc, s_mqttPayloadBuf, sizeof(s_mqttPayloadBuf));

    if (!mqttClient.publish(topic.c_str(), s_mqttPayloadBuf))
        logWarn("MQTT publish failed: " + topic);
}
/* ---------------------------------------------------------------------------
   BUILD DISCOVERY IDENTIFIERS
   ---------------------------------------------------------------------------
   Creates:
   - unique_id
   - object_id
   - friendly_name
   - discovery topic
--------------------------------------------------------------------------- */
void PyMqtt::buildDiscoveryIds(
    String& uniqueId,
    String& objectId,
    String& friendlyName,
    String& discoveryTopic,
    const String& subtopic,
    int moduleIndex,
    const String& displayName,
    int cellIndex,
    bool isCell
) {
    String prefix     = config.mqtt.prefix;
    String cellPrefix = config.mqtt.cellPrefix; // e.g. "Cell"

    // Base name: BAT5_Cell13_Voltage
    String base;
    if (isCell)
        base = subtopic + String(moduleIndex) + "_" +
               cellPrefix + String(cellIndex) + "_" + displayName;
    else
        base = subtopic + String(moduleIndex) + "_" + displayName;

    // unique_id: Pylontech_BAT5_Cell13_Voltage
    uniqueId = prefix + "_" + base;

    // object_id: BAT5_Cell13_Voltage
    objectId = base;

    // friendly_name: Pylontech BAT5 Cell13 Voltage
    if (isCell)
        friendlyName = prefix + " " + subtopic + String(moduleIndex) +
                       " " + cellPrefix + String(cellIndex) + " " + displayName;
    else
        friendlyName = prefix + " " + subtopic + String(moduleIndex) +
                       " " + displayName;

    // Discovery topic:
    // homeassistant/sensor/Pylontech_BAT5_Cell13_Voltage/config
    discoveryTopic =
        "homeassistant/sensor/" + uniqueId + "/config";
}

/* ---------------------------------------------------------------------------
   ADD DISCOVERY METADATA (unit, device_class, precision)
--------------------------------------------------------------------------- */
void PyMqtt::addDiscoveryMeta(
    JsonDocument& doc,
    const FieldConfig& fc
) {
    // Time fields → no metadata
    if (fc.factor == "date" || fc.unit == "timestamp")
        return;

    // Text fields (no unit, e.g. balancer "N"/"Y", state "Normal"/"Charge")
    // must NOT be declared as numeric measurement sensors: Home Assistant
    // rejects the non-numeric state for a state_class=measurement entity and
    // shows it as "Unknown". A unit-less field is treated as plain text.
    if (fc.unit.length() == 0 || fc.factor == "text")
        return;

    // Numeric fields
    int dec = decimalsForUnit(fc.unit);
    String devClass = deviceClassForUnit(fc.unit);

    if (devClass.length() > 0)
        doc["device_class"] = devClass;

    if (fc.unit.length() > 0)
        doc["unit_of_measurement"] = fc.unit;

    doc["state_class"] = "measurement";
    doc["suggested_display_precision"] = dec;
}

/* ---------------------------------------------------------------------------
   DISCOVERY: STACK
--------------------------------------------------------------------------- */
void PyMqtt::publishDiscoveryStack() {
    if (!enabled || !mqttClient.connected()) return;

    String prefix = config.mqtt.prefix;
    String sub    = config.mqtt.topicStack;
    String stateTopic = prefix + "/" + sub;

    // Cleaned prefix for IDs
    String prefixId = sanitizeId(prefix);

    StaticJsonDocument<256> docStack;
    docStack["StackVoltAvg"] = lastParsedStack.avgVoltage_mV / 1000.0f;
    docStack["StackCurrSum"] = lastParsedStack.totalCurrent_mA / 1000.0f;
    float stackPower = (lastParsedStack.avgVoltage_mV / 1000.0f) *
                       (lastParsedStack.totalCurrent_mA / 1000.0f);
    docStack["StackPower"] = stackPower;
    docStack["StackPowerIn"] = (stackPower > 0.0f) ? stackPower : 0.0f;
    docStack["StackPowerOut"] = (stackPower < 0.0f) ? stackPower : 0.0f;
    docStack["StackTempMax"] = lastParsedStack.temperature / 1000.0f;
    docStack["BatteryCount"] = lastParsedStack.batteryCount;
    docStack["SOC"] = lastParsedStack.soc;

    for (JsonPair kv : docStack.as<JsonObject>()) {

        String key = kv.key().c_str();   // z.B. "StackVoltAvg"
        String fullName = key;           // Anzeigename in HA

        // Build unique_id and entity_id from cleaned prefix + key
        String uniqueId = prefixId + "_stack_" + sanitizeId(key);

        String entityId = uniqueId;      // HA macht später sensor.<entityId>
        // entityId ist bereits lowercase/safe durch sanitizeId

        String discTopic = "homeassistant/sensor/" + uniqueId + "/config";

        StaticJsonDocument<512> doc;

        // Sichtbarer Name in HA (unverändert)
        doc["name"]   = fullName;
        doc["uniq_id"] = uniqueId;
        doc["obj_id"]  = entityId;

        doc["state_topic"]    = stateTopic;
        doc["value_template"] = "{{ value_json." + key + " }}";

        // Unit detection
        String unit = "";
        if (key == "StackVoltAvg") unit = "V";
        else if (key == "StackCurrSum") unit = "A";
        else if (key == "StackPower") unit = "W";
        else if (key == "StackPowerIn") unit = "W";
        else if (key == "StackPowerOut") unit = "W";
        else if (key == "StackTempMax") unit = "°C";
        else if (key == "SOC") unit = "%";
        else if (key == "BatteryCount") unit = "";

        // Suggested precision only if different from HA default (0)
        if (precisionDiffersFromDefault(unit)) {
            doc["suggested_display_precision"] = precisionForUnit(unit);
        }

        // Device class + unit
        if (unit == "V") {
            doc["device_class"] = "voltage";
            doc["unit_of_measurement"] = "V";
        }
        else if (unit == "A") {
            doc["device_class"] = "current";
            doc["unit_of_measurement"] = "A";
        }
        else if (unit == "W") {
            doc["device_class"] = "power";
            doc["unit_of_measurement"] = "W";
        }
        else if (unit == "°C") {
            doc["device_class"] = "temperature";
            doc["unit_of_measurement"] = "°C";
        }
        else if (unit == "%") {
            doc["device_class"] = "battery";
            doc["unit_of_measurement"] = "%";
        }

        doc["state_class"] = "measurement";

        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"]  = prefixId;          // Geräte-ID auch bereinigt
        dev["name"] = prefix + " Stack"; // sichtbarer Name unverändert

        String payload;
        serializeJson(doc, payload);

        mqttClient.publish(discTopic.c_str(), payload.c_str(), true);

        vTaskDelay(5);
    }
// ---------------------------------------------------------
// DISCOVERY: HealthState
// ---------------------------------------------------------
{
    String key = "HealthState";
    String uniqueId = prefixId + "_stack_healthstate";
    String discTopic = "homeassistant/sensor/" + uniqueId + "/config";

    StaticJsonDocument<512> doc;
    doc["name"] = "Health State";
    doc["uniq_id"] = uniqueId;
    doc["obj_id"] = uniqueId;

    doc["state_topic"] = stateTopic;
    doc["value_template"] = "{{ value_json.HealthState }}";

    doc["icon"] = "mdi:heart-pulse";
    doc["dev"]["ids"] = prefixId;
    doc["dev"]["name"] = prefix + " Stack";

    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
}

// ---------------------------------------------------------
// DISCOVERY: ModuleState
// ---------------------------------------------------------
{
    String key = "ModuleState";
    String uniqueId = prefixId + "_stack_modulestate";
    String discTopic = "homeassistant/sensor/" + uniqueId + "/config";

    StaticJsonDocument<512> doc;
    doc["name"] = "Module State";
    doc["uniq_id"] = uniqueId;
    doc["obj_id"] = uniqueId;

    doc["state_topic"] = stateTopic;
    doc["value_template"] = "{{ value_json.ModuleState }}";

    doc["icon"] = "mdi:battery-heart-variant";
    doc["dev"]["ids"] = prefixId;
    doc["dev"]["name"] = prefix + " Stack";

    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
}    
}

/* ---------------------------------------------------------------------------
   DISCOVERY: PWR MODULE
   ---------------------------------------------------------------------------
   - Visible names (doc["name"]) remain exactly as user entered them
   - unique_id, obj_id, dev.ids are sanitized for HA compatibility
   - JSON keys use normalizeName(fc.display) exactly like publishBat()
   - Text + Date fields send NO metadata (HA would reject them otherwise)
--------------------------------------------------------------------------- */
void PyMqtt::publishDiscoveryPwrModule(int moduleIndex) {
    if (!enabled || !mqttClient.connected()) return;

    String prefix      = config.mqtt.prefix;        // visible
    String prefixId    = sanitizeId(prefix);        // HA-safe
    String subtopic    = config.mqtt.topicPwr;      // visible
    String subtopicId  = sanitizeId(subtopic);      // HA-safe
    String stateTopic  = prefix + "/" + subtopic + "/" + String(moduleIndex);

    for (auto &kv : config.battery.fieldsPwr) {

        const String& fieldName = kv.first;
        const FieldConfig& fc   = kv.second;

        if (!fc.mqtt) continue;

        // JSON key used by publisher
        String displayKey = normalizeName(fc.display);

        // Visible name in HA
        String friendly = fc.display;

        // Build unique_id (HA-safe)
        String uniqueId =
            prefixId + "_" +
            subtopicId + "_" +
            String(moduleIndex) + "_" +
            sanitizeId(displayKey);

        // Entity ID (HA will prepend sensor.)
        String entityId = uniqueId;

        // Discovery topic
        String discTopic =
            "homeassistant/sensor/" + uniqueId + "/config";

        StaticJsonDocument<512> doc;

        // Visible name in HA (unchanged)
        doc["name"]    = friendly;
        doc["uniq_id"] = uniqueId;
        doc["obj_id"]  = entityId;

        // State topic
        doc["state_topic"] = stateTopic;

        // Template must match publisher JSON key
        doc["value_template"] =
            "{{ value_json." + displayKey + " }}";

        /* ---------------------------------------------------------
           TEXT FIELDS → no metadata at all
        --------------------------------------------------------- */
        if (fc.factor == "text") {
            // No device_class, no unit, no precision, no state_class
        }

        /* ---------------------------------------------------------
           DATE/TIMESTAMP → treat as text
           (Publisher already converts " " → "T")
        --------------------------------------------------------- */
        else if (fc.factor == "date" || fc.unit == "timestamp") {
            // Also no metadata
        }

        /* ---------------------------------------------------------
           NUMERIC FIELDS → normal metadata
        --------------------------------------------------------- */
        else {
            addDiscoveryMeta(doc, fc);
        }

        // Device block
        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"]  = prefixId + "_pwr_" + String(moduleIndex);
        dev["name"] = prefix + " PWR " + String(moduleIndex);

        // Publish retained
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(discTopic.c_str(), payload.c_str(), true);

        vTaskDelay(5);
    }
}
/* ---------------------------------------------------------------------------
   DISCOVERY: BAT CELL FIELD
--------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
   DISCOVERY: BAT CELL FIELD
   ---------------------------------------------------------------------------
   - Visible names (doc["name"]) remain exactly as user entered them
   - unique_id, obj_id, dev.ids are sanitized for HA compatibility
   - JSON keys use fc.display exactly like publishBatCells()
--------------------------------------------------------------------------- */
void PyMqtt::publishDiscoveryBatCell(int moduleIndex, int cellIndex) {
    if (!enabled || !mqttClient.connected()) return;

    String prefix      = config.mqtt.prefix;        // visible
    String prefixId    = sanitizeId(prefix);        // HA-safe
    String subtopic    = config.mqtt.topicBat;      // visible
    String subtopicId  = sanitizeId(subtopic);      // HA-safe

    String stateTopic =
        prefix + "/" + subtopic + "/" +
        String(moduleIndex) + "/" +
        config.mqtt.cellPrefix + String(cellIndex);

    for (auto &kv : config.battery.fieldsBat) {

        const String& fieldName = kv.first;
        const FieldConfig& fc   = kv.second;
        if (!fc.mqtt) continue;

        // JSON key used by publisher (BAT uses fc.display directly)
        String key = fc.display;

        // Visible name in HA
        String friendly = "Cell " + String(cellIndex) + " " + fc.display;

        // Build unique_id (HA-safe)
        String uniqueId =
            prefixId + "_" +
            subtopicId + "_" +
            String(moduleIndex) + "_cell" +
            String(cellIndex) + "_" +
            sanitizeId(key);

        // Entity ID (HA will prepend sensor.)
        String entityId = uniqueId;

        // Discovery topic
        String discTopic =
            "homeassistant/sensor/" + uniqueId + "/config";

        StaticJsonDocument<512> doc;

        // Visible name in HA (unchanged)
        doc["name"]    = friendly;
        doc["uniq_id"] = uniqueId;
        doc["obj_id"]  = entityId;

        // State topic
        doc["state_topic"] = stateTopic;

        // Template must match publisher JSON key
        doc["value_template"] =
            "{{ value_json." + key + " }}";

        // Unit + device_class + precision
        // Text fields (no unit, e.g. balancer "N"/"Y") must stay plain text:
        // a state_class=measurement sensor with a non-numeric state shows up as
        // "Unknown" in Home Assistant.
        bool isNumeric =
            fc.unit.length() > 0 &&
            fc.factor != "text" &&
            fc.factor != "date" &&
            fc.unit   != "timestamp";

        if (isNumeric) {
            if (precisionDiffersFromDefault(fc.unit))
                doc["suggested_display_precision"] = precisionForUnit(fc.unit);

            if (fc.unit == "V")  doc["device_class"] = "voltage";
            if (fc.unit == "A")  doc["device_class"] = "current";
            if (fc.unit == "°C") doc["device_class"] = "temperature";
            if (fc.unit == "%")  doc["device_class"] = "battery";
            if (fc.unit == "Ah") doc["device_class"] = "energy";

            if (fc.unit.length() > 0)
                doc["unit_of_measurement"] = fc.unit;

            doc["state_class"] = "measurement";
        }

        // Device block
        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"]  = prefixId + "_bat_" + String(moduleIndex);
        dev["name"] = prefix + " BAT " + String(moduleIndex);

        // Publish retained
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(discTopic.c_str(), payload.c_str(), true);

        vTaskDelay(5);
    }
}
/* ---------------------------------------------------------------------------
   DISCOVERY: STAT FIELD
--------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
   DISCOVERY: STAT MODULE
   ---------------------------------------------------------------------------
   - Visible names (doc["name"]) remain exactly as user entered them
   - unique_id, obj_id, dev.ids are sanitized for HA compatibility
   - JSON keys use normalizeName(fc.display) exactly like publishStat()
--------------------------------------------------------------------------- */
void PyMqtt::publishDiscoveryStatModule(int moduleIndex) {
    if (!enabled || !mqttClient.connected()) return;
    if (!config.battery.enableStat) return;

    String prefix      = config.mqtt.prefix;        // visible
    String prefixId    = sanitizeId(prefix);        // HA-safe
    String subtopic    = config.mqtt.topicStat;     // visible
    String subtopicId  = sanitizeId(subtopic);      // HA-safe

    String stateTopic =
        prefix + "/" + subtopic + "/" + String(moduleIndex);

    for (auto &kv : config.battery.fieldsStat) {

        const String& fieldName = kv.first;
        const FieldConfig& fc   = kv.second;
        if (!fc.mqtt) continue;

        // JSON key used by publisher
        String displayKey = normalizeName(fc.display);

        // Visible name in HA
        String friendly = fc.display;

        // Build unique_id (HA-safe)
        String uniqueId =
            prefixId + "_" +
            subtopicId + "_" +
            String(moduleIndex) + "_" +
            sanitizeId(displayKey);

        // Entity ID (HA will prepend sensor.)
        String entityId = uniqueId;

        // Discovery topic
        String discTopic =
            "homeassistant/sensor/" + uniqueId + "/config";

        StaticJsonDocument<512> doc;

        // Visible name in HA (unchanged)
        doc["name"]    = friendly;
        doc["uniq_id"] = uniqueId;
        doc["obj_id"]  = entityId;

        // State topic
        doc["state_topic"] = stateTopic;

        // Template must match publisher JSON key
        doc["value_template"] =
            "{{ value_json." + displayKey + " }}";

        // Unit + device_class + precision
        addDiscoveryMeta(doc, fc);

        // Device block
        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"]  = prefixId + "_stat_" + String(moduleIndex);
        dev["name"] = prefix + " STAT " + String(moduleIndex);

        // Publish retained
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(discTopic.c_str(), payload.c_str(), true);

        vTaskDelay(5);
    }
}

/* ---------------------------------------------------------------------------
   PUBLISH INFO JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishInfo(int moduleIndex, const InfoData& info) {
    if (!enabled || !mqttClient.connected()) return;
    if (info.fields.empty()) return;

    String subtopic = config.mqtt.topicInfo;
    String topic = config.mqtt.prefix + "/" + subtopic + "/" + String(moduleIndex);

    StaticJsonDocument<1024> doc;

    for (auto &f : info.fields) {

        if (!config.battery.fieldsInfo.count(f.name)) continue;
        const FieldConfig &fc = config.battery.fieldsInfo[f.name];
        if (!fc.mqtt) continue;

        // Some firmwares return placeholder "0" for textual INFO fields.
        // Skip those to avoid publishing misleading values.
        String rawTrim = f.raw;
        rawTrim.trim();
        bool isTextLike = (fc.factor == "text" || fc.factor == "date" || fc.unit == "timestamp");
        if (isTextLike && (rawTrim.length() == 0 || rawTrim == "0")) continue;

        String display = normalizeName(fc.display);
        String value   = computeValue(f.raw, fc);
        if (value.length() == 0) continue;

        setTypedJsonValue(doc, display, value, fc);
    }

    if (doc.size() == 0) return;  // keine MQTT-Felder konfiguriert

    String payload;
    serializeJson(doc, payload);

    if (!mqttClient.publish(topic.c_str(), payload.c_str()))
        logWarn("MQTT publish failed: " + topic);
}

/* ---------------------------------------------------------------------------
   DISCOVERY: INFO MODULE
--------------------------------------------------------------------------- */
void PyMqtt::publishDiscoveryInfoModule(int moduleIndex) {
    if (!enabled || !mqttClient.connected()) return;
    if (!config.battery.enableInfo) return;

    String prefix     = config.mqtt.prefix;
    String prefixId   = sanitizeId(prefix);
    String subtopic   = config.mqtt.topicInfo;
    String subtopicId = sanitizeId(subtopic);

    String stateTopic =
        prefix + "/" + subtopic + "/" + String(moduleIndex);

    for (auto &kv : config.battery.fieldsInfo) {

        const FieldConfig& fc = kv.second;
        if (!fc.mqtt) continue;

        String displayKey = normalizeName(fc.display);
        String friendly   = fc.display;

        String uniqueId =
            prefixId + "_" +
            subtopicId + "_" +
            String(moduleIndex) + "_" +
            sanitizeId(displayKey);

        String discTopic =
            "homeassistant/sensor/" + uniqueId + "/config";

        StaticJsonDocument<512> doc;

        doc["name"]           = friendly;
        doc["uniq_id"]        = uniqueId;
        doc["obj_id"]         = uniqueId;
        doc["state_topic"]    = stateTopic;
        doc["value_template"] = "{{ value_json." + displayKey + " }}";

        addDiscoveryMeta(doc, fc);

        JsonObject dev = doc.createNestedObject("dev");
        dev["ids"]  = prefixId + "_info_" + String(moduleIndex);
        dev["name"] = prefix + " INFO " + String(moduleIndex);

        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(discTopic.c_str(), payload.c_str(), true);

        vTaskDelay(5);
    }
}