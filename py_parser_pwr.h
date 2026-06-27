#pragma once
#include <Arduino.h>
#include <vector>
#include <map>
#include "config.h"

// ---------------------------------------------------------
// PWR Parser Header
// ---------------------------------------------------------
// - Stack mode: behaves exactly as before
// - Hub mode: supports Hub + Masterhub (Address/Stack/Module)
// - Returns one "overall" BatteryStack via stackOut
// - Exposes detailed hub/stack data via globals
// ---------------------------------------------------------

// Main PWR parser function (entry point)
ParseResult parsePwrFrame(const String& raw,
                          BatteryStack& stackOut,
                          std::vector<BatteryModule>& modulesOut);

// Global parser results for Web UI (as before)
extern BatteryStack lastParsedStack;
extern std::vector<BatteryModule> lastParsedModules;
extern std::vector<String> lastParserHeader;
extern std::vector<String> lastParserValues;

// New: detailed hub/stack result
// hubs[hubID][stackID] = BatteryStack
struct ParsedHubData {
    std::map<int, std::map<int, BatteryStack>> hubs;
};

extern ParsedHubData lastParsedHub;
