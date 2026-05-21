#pragma once
#include <Arduino.h>
#include <vector>
#include <map>
#include "config.h"
#include "py_parser_pwr.h"

class PyMqttHub {
public:
    // Publish all stacks (Hub or Masterhub)
    void publishHubStacks(const ParsedHubData& hubData);

    // Publish all modules (Hub or Masterhub)
    void publishHubModules(const std::vector<BatteryModule>& modules);

    // Publish Home Assistant discovery for Hub mode
    void publishHubDiscovery(const ParsedHubData& hubData);

private:
    // Build MQTT topic for a stack
    String buildStackTopic(int hubID, int stackID);

    // Build MQTT topic for a module
    String buildModuleTopic(int hubID, int stackID, int moduleID);

    // Build discovery topic
    String buildDiscoveryTopic(const String& uniqueId);

    // Build unique ID for HA
    String buildUniqueId(int hubID, int stackID, const String& name);

    // Build unique ID for module
    String buildModuleUniqueId(int hubID, int stackID, int moduleID, const String& name);

    // Helper: sanitize for HA
    String sanitize(const String& in);
};
