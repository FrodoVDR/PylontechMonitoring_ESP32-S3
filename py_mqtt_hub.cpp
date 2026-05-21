#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "py_parser_pwr.h"
#include "py_mqtt.h"
#include "py_mqtt_hub.h"
#include "py_log.h"

// Single global instance is created in py_mqtt.cpp:
//   static PyMqttHub py_mqtt_hub;
extern PyMqtt py_mqtt;

// ---------------------------------------------------------
// Helper: sanitize string for Home Assistant IDs
// ---------------------------------------------------------
String PyMqttHub::sanitize(const String& in) {
    String out;
    out.reserve(in.length());
    for (char c : in) {
        if (isalnum((unsigned char)c)) {
            out += (char)tolower((unsigned char)c);
        } else {
            out += '_';
        }
    }
    return out;
}

// ---------------------------------------------------------
// Build stack topic: prefix/HubX/StackY
// ---------------------------------------------------------
String PyMqttHub::buildStackTopic(int hubID, int stackID) {
    return config.mqtt.prefix +
           "/Hub" + String(hubID) +
           "/Stack" + String(stackID);
}

// ---------------------------------------------------------
// Build module topic: prefix/HubX/StackY/ModuleZ
// ---------------------------------------------------------
String PyMqttHub::buildModuleTopic(int hubID, int stackID, int moduleID) {
    return config.mqtt.prefix +
           "/Hub" + String(hubID) +
           "/Stack" + String(stackID) +
           "/Module" + String(moduleID);
}

// ---------------------------------------------------------
// Build discovery topic: homeassistant/sensor/<uid>/config
// ---------------------------------------------------------
String PyMqttHub::buildDiscoveryTopic(const String& uniqueId) {
    return "homeassistant/sensor/" + uniqueId + "/config";
}

// ---------------------------------------------------------
// Build unique ID for stack
// ---------------------------------------------------------
String PyMqttHub::buildUniqueId(int hubID, int stackID, const String& name) {
    return sanitize(config.mqtt.prefix) +
           "_hub" + String(hubID) +
           "_stack" + String(stackID) +
           "_" + sanitize(name);
}

// ---------------------------------------------------------
// Build unique ID for module
// ---------------------------------------------------------
String PyMqttHub::buildModuleUniqueId(int hubID, int stackID, int moduleID, const String& name) {
    return sanitize(config.mqtt.prefix) +
           "_hub" + String(hubID) +
           "_stack" + String(stackID) +
           "_module" + String(moduleID) +
           "_" + sanitize(name);
}

// ---------------------------------------------------------
// Publish all stacks (Hub or Masterhub)
// ---------------------------------------------------------
void PyMqttHub::publishHubStacks(const ParsedHubData& hubData) {
    // Only publish if MQTT is connected
    for (auto& hubPair : hubData.hubs) {
        int hubID = hubPair.first;

        for (auto& stackPair : hubPair.second) {
            int stackID = stackPair.first;
            const BatteryStack& s = stackPair.second;

            String topic = buildStackTopic(hubID, stackID);

            StaticJsonDocument<256> doc;
            doc["AvgVolt"] = s.avgVoltage_mV / 1000.0f;
            doc["SumCurr"] = s.totalCurrent_mA / 1000.0f;
            doc["TempMax"] = s.temperature / 1000.0f;
            doc["SOC"]     = s.soc;
            doc["Count"]   = s.batteryCount;

            String payload;
            serializeJson(doc, payload);

            py_mqtt.publishDirect(topic, payload);
        }
    }
}

// ---------------------------------------------------------
// Publish all modules (Hub or Masterhub)
// ---------------------------------------------------------
void PyMqttHub::publishHubModules(const std::vector<BatteryModule>& modules) {
    for (const auto& m : modules) {
        if (!m.present) continue;

        String topic = buildModuleTopic(m.hub, m.stack, m.index);

        StaticJsonDocument<512> doc;
        // Publish raw fields as-is; can be refined later
        for (auto& kv : m.fields) {
            doc[kv.first] = kv.second;
        }

        String payload;
        serializeJson(doc, payload);

        py_mqtt.publishDirect(topic, payload);
    }
}

// ---------------------------------------------------------
// Publish Home Assistant discovery for Hub mode
// ---------------------------------------------------------
void PyMqttHub::publishHubDiscovery(const ParsedHubData& hubData) {
    String prefix   = config.mqtt.prefix;      // visible name
    String prefixId = sanitize(prefix);        // HA-safe ID

    // -----------------------------------------------------
    // 1) DEVICE DISCOVERY PER HUB
    // -----------------------------------------------------
    for (auto& hubPair : hubData.hubs) {
        int hubID = hubPair.first;

        String hubUid      = prefixId + "_hub" + String(hubID);
        String hubDiscTopic = "homeassistant/device_tracker/" + hubUid + "/config";

        StaticJsonDocument<512> dev;
        dev["name"]    = prefix + " Hub " + String(hubID);
        dev["uniq_id"] = hubUid;

        JsonObject devObj = dev.createNestedObject("dev");
        devObj["ids"]  = hubUid;
        devObj["name"] = prefix + " Hub " + String(hubID);

        String payload;
        serializeJson(dev, payload);
        py_mqtt.publishDirect(hubDiscTopic, payload);

        // -------------------------------------------------
        // 2) STACK DISCOVERY PER HUB
        // -------------------------------------------------
        for (auto& stackPair : hubPair.second) {
            int stackID = stackPair.first;

            String stackUid      = prefixId + "_hub" + String(hubID) + "_stack" + String(stackID);
            String stackDiscTopic = "homeassistant/sensor/" + stackUid + "/config";
            String stateTopic     = buildStackTopic(hubID, stackID);

            StaticJsonDocument<512> doc;
            doc["name"]        = "Hub " + String(hubID) + " Stack " + String(stackID);
            doc["uniq_id"]     = stackUid;
            doc["state_topic"] = stateTopic;

            JsonObject devObj2 = doc.createNestedObject("dev");
            devObj2["ids"]  = hubUid;
            devObj2["name"] = prefix + " Hub " + String(hubID);

            String payload2;
            serializeJson(doc, payload2);
            py_mqtt.publishDirect(stackDiscTopic, payload2);
        }
    }

    // -----------------------------------------------------
    // 3) MODULE DISCOVERY
    // -----------------------------------------------------
    extern std::vector<BatteryModule> lastParsedModules;

    for (auto& m : lastParsedModules) {
        if (!m.present) continue;

        int hubID   = m.hub;
        int stackID = m.stack;

        String modUid = buildModuleUniqueId(hubID, stackID, m.index, "module");

        String discTopic  = buildDiscoveryTopic(modUid);
        String stateTopic = buildModuleTopic(hubID, stackID, m.index);

        StaticJsonDocument<512> doc;
        doc["name"]        = "Hub " + String(hubID) +
                             " Stack " + String(stackID) +
                             " Module " + String(m.index);
        doc["uniq_id"]     = modUid;
        doc["state_topic"] = stateTopic;

        JsonObject devObj = doc.createNestedObject("dev");
        devObj["ids"]  = sanitize(prefix) + "_hub" + String(hubID);
        devObj["name"] = prefix + " Hub " + String(hubID);

        String payload;
        serializeJson(doc, payload);
        py_mqtt.publishDirect(discTopic, payload);
    }
}
