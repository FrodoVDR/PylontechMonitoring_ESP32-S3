#pragma once

#include "py_log.h"
#include "config.h"
#include "py_parser_pwr.h"
#include "py_parser_bat.h"
#include "py_parser_stat.h"
#include "py_mqtt.h"
#include "py_mqtt_hub.h"
#include <WiFi.h>
#include <map>
#include <set>

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

// Discovery triggers
bool discoveryPwrNeeded   = false;
bool discoveryBatNeeded   = false;
bool discoveryStatNeeded  = false;

// Discovery tracking (modules already announced)
std::set<int> discoveredPwr;
std::set<int> discoveredBat;
std::set<int> discoveredStat;

// Discovery state machine
enum DiscoveryPhase {
    DISC_IDLE,
    DISC_STACK,
    DISC_PWR,
    DISC_BAT,
    DISC_STAT,
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

// Reconnect timers
static unsigned long lastReconnectAttempt = 0;
static unsigned long wifiConnectedSince   = 0;

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

/* ---------------------------------------------------------------------------
   DISCOVERY RESET
   ---------------------------------------------------------------------------
   Resets the discovery state machine and clears module tracking.
--------------------------------------------------------------------------- */
void PyMqtt::resetDiscovery(bool pwr, bool bat, bool stat) {

    if (pwr) discoveredPwr.clear();
    if (bat) discoveredBat.clear();
    if (stat) discoveredStat.clear();

    discoveryPhase = DISC_STACK;
    discPwrIndex   = 0;
    discBatModule  = 0;
    discBatCell    = 0;
    discBatField   = 0;
    discStatModule = 0;
    discStatField  = 0;

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

    // Double-buffer
    PwrBuffer* pwr  = pwrUseA  ? &pwrA  : &pwrB;
    BatBuffer* bat  = batUseA  ? &batA  : &batB;
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

    if (parserHasData) {

        if (g_batteryMode == BatteryMode::STACK) {
            // Original behavior
            publishStack(pwr->stack);

            for (const auto& mod : pwr->modules) {
                if (!mod.present) continue;
                publishBat(mod.index, mod);
            }
        }
        else if (g_batteryMode == BatteryMode::HUB) {
            // NEW: Hub publishing
            py_mqtt_hub.publishHubStacks(lastParsedHub);
            py_mqtt_hub.publishHubModules(pwr->modules);
        }

        parserHasData = false;
    }

    // ---------------------------------------------------------
    // PUBLISH BAT CELLS
    // ---------------------------------------------------------
    if (batParserHasData) {
        publishBatCells(batParserModuleIndex, bat->cells);
        batParserHasData = false;
    }

    // ---------------------------------------------------------
    // PUBLISH STAT
    // ---------------------------------------------------------
    if (statParserHasData) {
        publishStat(statParserModuleIndex, stat->stat);
        statParserHasData = false;
    }

    // ---------------------------------------------------------
    // DISCOVERY STATE MACHINE
    // ---------------------------------------------------------
    handleDiscoveryStep(*pwr, *bat, *stat);

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
                return;
            }
            for (size_t i = 0; i < bat.cells.size(); i++) {
                publishDiscoveryBatCell(pwr.modules[discBatModule].index, i);
                vTaskDelay(5);
            }
            discBatModule++;
            return;

        case DISC_STAT:
            if (discStatModule >= pwr.modules.size()) {
                discoveryPhase = DISC_DONE;
                return;
            }
            if (!pwr.modules[discStatModule].present) {
                discStatModule++;
                return;
            }
            publishDiscoveryStatModule(pwr.modules[discStatModule].index);
            discStatModule++;
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
    if (!enabled || !mqttClient.connected() || !parserHasData) return;

    String topic = config.mqtt.prefix + "/" + config.mqtt.topicStack;

    StaticJsonDocument<256> doc;
    doc["StackVoltAvg"] = stack.avgVoltage_mV / 1000.0f;
    doc["StackCurrSum"] = stack.totalCurrent_mA / 1000.0f;
    doc["StackTempMax"] = stack.temperature / 1000.0f;
    doc["BatteryCount"] = stack.batteryCount;

    // Health-State
    doc["HealthState"] = getStackHealthState();

    // Kompakter Modulstatus
    doc["ModuleState"] = getModuleStatusString();

    String payload;
    serializeJson(doc, payload);

    mqttClient.publish(topic.c_str(), payload.c_str());
}


/* ---------------------------------------------------------------------------
   PUBLISH PWR MODULE JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishBat(int index, const BatteryModule& mod) {
    if (!enabled || !mqttClient.connected() || !parserHasData || !mod.present)
        return;

    String subtopic = config.mqtt.topicPwr;
    String topic = config.mqtt.prefix + "/" + subtopic + "/" + String(index);

    StaticJsonDocument<512> doc;

    for (auto &kv : config.battery.fieldsPwr) {
        const String& fieldName = kv.first;
        const FieldConfig& fc = kv.second;

        if (!fc.mqtt) continue;

        auto it = mod.fields.find(fieldName);
        if (it == mod.fields.end()) continue;

        String display = normalizeName(fc.display);
        String value = computeValue(it->second, fc);

        doc[display] = value.c_str();

    }

    config.lastMqttContact = config.getCurrentTimeString();

    String payload;
    serializeJson(doc, payload);

    if (!mqttClient.publish(topic.c_str(), payload.c_str()))
        logWarn("MQTT publish failed: " + topic);
}

/* ---------------------------------------------------------------------------
   PUBLISH BAT CELLS JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishBatCells(int moduleIndex, const std::vector<BatData>& batCells) {
    if (!enabled || !mqttClient.connected()) return;
    if (batCells.empty()) return;

    String subtopic = config.mqtt.topicBat;

    for (const auto& cell : batCells) {

        StaticJsonDocument<512> doc;

        for (auto &f : cell.fields) {

            // Feld existiert in der BAT-Konfiguration?
            if (!config.battery.fieldsBat.count(f.name)) continue;
            const FieldConfig &fc = config.battery.fieldsBat[f.name];
            if (!fc.mqtt) continue;

            // WICHTIG:
            // Discovery benutzt fc.display als JSON-Key → Publisher muss das auch tun
            String key   = fc.display;
            String value = computeValue(f.raw, fc);

            doc[key] = value;
        }

        String payload;
        serializeJson(doc, payload);

        String topic =
            config.mqtt.prefix + "/" + subtopic + "/" +
            String(moduleIndex) + "/" +
            config.mqtt.cellPrefix + String(cell.cellIndex);

        mqttClient.publish(topic.c_str(), payload.c_str());
    }
}

/* ---------------------------------------------------------------------------
   PUBLISH STAT JSON
--------------------------------------------------------------------------- */
void PyMqtt::publishStat(int moduleIndex, const StatData& stat) {
    if (!enabled || !mqttClient.connected()) return;
    if (!config.battery.enableStat) return;
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

        doc[display] = value.c_str();

    }

    String payload;
    serializeJson(doc, payload);

    if (!mqttClient.publish(topic.c_str(), payload.c_str()))
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
    docStack["StackTempMax"] = lastParsedStack.temperature / 1000.0f;
    docStack["BatteryCount"] = lastParsedStack.batteryCount;

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
        else if (key == "StackTempMax") unit = "°C";
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
        else if (unit == "°C") {
            doc["device_class"] = "temperature";
            doc["unit_of_measurement"] = "°C";
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
        bool isNumeric =
            !(fc.factor == "text" ||
              fc.factor == "date" ||
              fc.unit   == "timestamp");

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