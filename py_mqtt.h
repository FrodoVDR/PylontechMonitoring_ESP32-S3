#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "py_parser_pwr.h"


// Forward declarations
struct BatteryModule;
struct BatteryStack;
struct BatField;
struct BatData;
struct StatField;
struct StatData;
struct InfoField;
struct InfoData;
struct ParsedData;

struct MqttMessage {
    char topic[128];
    char payload[64];
};

class PyMqtt {
public:
    // Core lifecycle
    void begin();
    void loop();
    void resetDiscovery(bool pwr, bool bat, bool stat);

    // Raw publish (Task1 → Task2)
    bool publishRaw(const String& topic, const String& payload);

    // Publish parsed data
    void publishStack(const BatteryStack& stack);
    void publishBat(int index, const BatteryModule& mod);
    void publishDiscoveryBatModule(int moduleIndex);
    void publishDiscoveryBatCell(int moduleIndex, int cellIndex);
    void publishBatCells(int moduleIndex, const std::vector<BatData>& batCells);
    void publishStat(int moduleIndex, const StatData& stat);
    void publishInfo(int moduleIndex, const InfoData& info);

    bool isDiscoveryActive() const { return discoveryActive; }

    bool isConnected() { return mqttClient.connected(); }
    void publishDiscoveryStatModule(int moduleIndex);
    void publishDiscoveryInfoModule(int moduleIndex);
    bool publishDirect(const String& topic, const String& payload);



private:
    WiFiClient   wifiClient;
    PubSubClient mqttClient = PubSubClient(wifiClient);

    bool enabled = false;
    bool discoveryActive = false;

    int precisionForUnit(const String& unit);
    bool precisionDiffersFromDefault(const String& unit);

    // MQTT connection
    bool connect();

    // Value conversion (numeric, text, date)
    String computeValue(const String& raw, const FieldConfig& fc);

    // Name normalization (CamelCase)
    String normalizeName(const String& in);

    // Decimal precision based on unit
    int decimalsForUnit(const String& unit);

    // Device class based on unit
    String deviceClassForUnit(const String& unit);

    // Build MQTT topic
    String buildTopic(
        const String& subtopic,
        int moduleIndex,
        const String& fieldName,
        int cellIndex,
        bool isCell
    );

    // Build unique_id, object_id, friendly_name, discovery topic
    void buildDiscoveryIds(
        String& uniqueId,
        String& objectId,
        String& friendlyName,
        String& discoveryTopic,
        const String& subtopic,
        int moduleIndex,
        const String& displayName,
        int cellIndex,
        bool isCell
    );

    // Add HA metadata (unit, device_class, precision)
    void addDiscoveryMeta(JsonDocument& doc, const FieldConfig& fc);

    // Discovery publishers
    void publishDiscoveryStack();
    void publishDiscoveryPwrModule(int moduleIndex);
    void publishDiscoveryStatField(int moduleIndex, const StatField& f);
    void publishDiscoveryInfoField(int moduleIndex, const InfoField& f);

    // Discovery state machine
    void handleDiscoveryStep(
        const PwrBuffer& pwr,
        const BatBuffer& bat,
        const StatBuffer& stat
    );

    // Logging helper
    void logPublishFailure(const String& topic);
};