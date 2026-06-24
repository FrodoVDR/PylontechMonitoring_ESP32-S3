#include "py_parser_pwr.h"
#include "py_log.h"
#include "config.h"
#include "py_uart.h"

#include <cctype>
#include <algorithm>
#include <map>
#include <cstdlib>

// UART instance
extern PyUart py_uart;

// Global parser data for Web UI
BatteryStack lastParsedStack;
std::vector<BatteryModule> lastParsedModules;
std::vector<String> lastParserHeader;
std::vector<String> lastParserValues;

// New: hub/stack result
ParsedHubData lastParsedHub;

// External battery mode (must exist somewhere in your code)
extern BatteryMode g_batteryMode;

// ---------------------------------------------------------
// Helper: trim whitespace
// ---------------------------------------------------------
static String trimWS(const String& s) {
    String r = s;
    r.trim();
    return r;
}

// Keep only numeric prefix if a value contains a unit suffix.
static String extractLeadingNumber(const String& raw) {
    String t = raw;
    t.trim();
    if (t.length() == 0) return t;

    const char* s = t.c_str();
    char* endPtr = nullptr;
    strtof(s, &endPtr);
    if (endPtr == s || endPtr == nullptr) {
        return t;
    }

    const char* p = endPtr;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') {
        return String(s).substring(0, (int)(endPtr - s));
    }

    bool suffixLooksLikeUnit = true;
    for (const char* q = p; *q != '\0'; ++q) {
        char c = *q;
        if (isalpha((unsigned char)c) || c == '%' || c == '/' || c == '_' || c == '-' || c == '.' || c == ' ') {
            continue;
        }
        suffixLooksLikeUnit = false;
        break;
    }

    if (suffixLooksLikeUnit) {
        return String(s).substring(0, (int)(endPtr - s));
    }

    return t;
}

// Normalize SOC to percent range 0..100.
static int normalizeSocPercent(const String& raw) {
    String n = extractLeadingNumber(raw);
    n.trim();
    if (n.length() == 0) return 0;

    float v = n.toFloat();

    // Some firmwares report SOC-like values scaled by 1000 (e.g. 30673 => 30.673%).
    if (v > 100.0f && v <= 100000.0f) {
        v /= 1000.0f;
    }

    if (v < 0.0f) v = 0.0f;
    if (v > 100.0f) v = 100.0f;
    return (int)(v + 0.5f);
}

// ---------------------------------------------------------
// Helper: split by whitespace
// ---------------------------------------------------------
static std::vector<String> splitWS(const String& line) {
    std::vector<String> out;
    int start = 0;

    while (start < line.length()) {
        while (start < line.length() && isspace((unsigned char)line[start])) start++;
        if (start >= line.length()) break;

        int end = start;
        while (end < line.length() && !isspace((unsigned char)line[end])) end++;

        out.push_back(line.substring(start, end));
        start = end;
    }
    return out;
}

// ---------------------------------------------------------
// Extract @ ... $$ frame
// ---------------------------------------------------------
static bool extractFrame(const String& raw, String& frame) {
    int start = raw.indexOf('@');
    if (start < 0) return false;
    // Search the terminator strictly after '@' so a '$$' inside a preceding
    // asynchronous log line cannot mis-bound the frame.
    int end = raw.indexOf("$$", start + 1);

    if (end < 0 || end <= start)
        return false;

    frame = raw.substring(start + 1, end);
    return true;
}

// ---------------------------------------------------------
// Forward declarations for mode-specific parsers
// ---------------------------------------------------------
static ParseResult parsePwrFrameStackMode(const String& raw,
                                          BatteryStack& stackOut,
                                          std::vector<BatteryModule>& modulesOut);

static ParseResult parsePwrFrameHubMode(const String& raw,
                                        BatteryStack& stackOut,
                                        std::vector<BatteryModule>& modulesOut);

// ---------------------------------------------------------
// Main PWR parser (mode dispatcher)
// ---------------------------------------------------------
ParseResult parsePwrFrame(const String& raw,
                          BatteryStack& stackOut,
                          std::vector<BatteryModule>& modulesOut)
{
    // Only handle frames that belong to "pwr"
    if (py_uart.getLastCommand() != "pwr") {
        Log(LOG_DEBUG, "PWR parser: ignoring frame (last command was '" + py_uart.getLastCommand() + "')");
        return PARSE_IGNORED;
    }

    // Only handle valid frames
    if (!py_uart.isFrameValid()) {
        Log(LOG_WARN, "PWR parser: skipping invalid frame");
        return PARSE_FAIL;
    }

    modulesOut.clear();
    stackOut.reset();
    lastParsedHub.hubs.clear();

    Log(LOG_INFO, "PWR parser: raw frame received, length=" + String(raw.length()));

    // Dispatch by battery mode
    if (g_batteryMode == BatteryMode::STACK) {
        return parsePwrFrameStackMode(raw, stackOut, modulesOut);
    }
    else if (g_batteryMode == BatteryMode::HUB) {
        return parsePwrFrameHubMode(raw, stackOut, modulesOut);
    }
    else {
        Log(LOG_WARN, "PWR parser: unknown battery mode, skipping");
        return PARSE_FAIL;
    }
}

// ---------------------------------------------------------
// Common: split frame into lines and header
// ---------------------------------------------------------
static bool splitFrameLinesAndHeader(const String& rawFrame,
                                     std::vector<String>& lines,
                                     std::vector<String>& header)
{
    lines.clear();
    header.clear();

    int pos = 0;
    while (true) {
        int nl = rawFrame.indexOf('\n', pos);
        if (nl < 0) {
            String last = trimWS(rawFrame.substring(pos));
            if (last.length() > 0) lines.push_back(last);
            break;
        }
        String line = trimWS(rawFrame.substring(pos, nl));
        if (line.length() > 0) lines.push_back(line);
        pos = nl + 1;
    }

    if (lines.size() < 2) {
        Log(LOG_WARN, "PWR parser: too few lines");
        return false;
    }

    header = splitWS(lines[0]);
    if (header.size() < 3) {
        Log(LOG_WARN, "PWR parser: header too small");
        return false;
    }

    lastParserHeader = header;
    lastParserValues.clear();
    return true;
}

// ---------------------------------------------------------
// STACK MODE PARSER (original behavior, minimal changes)
// ---------------------------------------------------------
static ParseResult parsePwrFrameStackMode(const String& raw,
                                          BatteryStack& stackOut,
                                          std::vector<BatteryModule>& modulesOut)
{
    String frame;
    if (!extractFrame(raw, frame)) {
        Log(LOG_WARN, "PWR parser (STACK): no valid @ ... $$ frame found");
        return PARSE_FAIL;
    }

    std::vector<String> lines;
    std::vector<String> header;
    if (!splitFrameLinesAndHeader(frame, lines, header)) {
        return PARSE_FAIL;
    }

    int baseIndex = -1;
    int timeIndex = -1;

    for (size_t h = 0; h < header.size(); h++) {
        if (header[h] == "Base.St" || header[h] == "Base") {
            baseIndex = h;
        }
        if (header[h] == "Time") {
            timeIndex = h;
        }
    }

    for (size_t i = 1; i < lines.size(); i++) {

        std::vector<String> cols = splitWS(lines[i]);
        if (cols.empty()) continue;

        // Merge date + time if split
        if (timeIndex >= 0 && cols.size() > (size_t)timeIndex + 1) {
            String datePart = cols[timeIndex];
            String timePart = cols[timeIndex + 1];

            bool looksLikeDate = (datePart.indexOf('-') > 0 || datePart.indexOf('/') > 0);
            bool looksLikeTime = (timePart.indexOf(':') > 0);

            if (looksLikeDate && looksLikeTime) {
                cols[timeIndex] = datePart + " " + timePart;
                cols.erase(cols.begin() + timeIndex + 1);
            }
        }

        if (cols.size() < header.size()) continue;

        // Absent → end of list
        if (baseIndex >= 0 && cols[baseIndex] == "Absent") {
            Log(LOG_INFO, "PWR parser (STACK): Absent detected at line " + String(i));
            break;
        }

        // First valid line for Web UI
        if (lastParserValues.empty()) {
            lastParserValues = cols;
        }

        BatteryModule mod;
        mod.present = true;
        mod.hub = 0;
        mod.stack = 0;

        mod.fields.reserve(header.size());
        for (size_t c = 0; c < header.size(); c++) {
            const String& col = header[c];
            const String& value = cols[c];

            mod.fields[col] = value;

            if (col == "Power" || col == "Battery") {
                mod.index = value.toInt();
            }
            else if (col == "Volt") {
                mod.voltage_mV = value.toInt();
            }
            else if (col == "Curr") {
                mod.current_mA = value.toInt();
            }
            else if (col == "Tempr") {
                mod.temperature = value.toInt();
            }
            else if (col == "Coulomb") {
                String v = extractLeadingNumber(value);
                mod.fields[col] = v;
                // Coulomb is used as SOC source on some firmware variants.
                mod.soc = normalizeSocPercent(v);
            }
            else if (col == "SOC") {
                mod.soc = normalizeSocPercent(value);
                mod.fields[col] = String(mod.soc);
            }
        }

        bool plausible = true;
        plausible &= (mod.index >= 1 && mod.index <= 32);
        plausible &= (mod.voltage_mV > 10000 && mod.voltage_mV < 60000);

        if (!plausible) {
            Log(LOG_WARN, "PWR parser (STACK): skipping implausible module line " + String(i));
            continue;
        }

        // Keep module row even when optional fields are missing/noisy.
        if (mod.temperature < 0 || mod.temperature > 60000) mod.temperature = 0;
        if (mod.soc < 0) mod.soc = 0;
        if (mod.soc > 100) mod.soc = 100;

        modulesOut.push_back(mod);
    }

    if (modulesOut.empty()) {
        Log(LOG_WARN, "PWR parser (STACK): no modules parsed");
        return PARSE_FAIL;
    }

    // Stack calculation
    int count = (int)modulesOut.size();

    // Module count uses a high-water mark: once N modules have been detected,
    // that count persists (clamped to 1..maxModules) and is not collapsed by a
    // single shorter parse. This is the count published via MQTT and shown on
    // the dashboard.
    int maxModules = (int)config.battery.maxModules;
    if (maxModules < 1)  maxModules = 16;
    if (maxModules > 16) maxModules = 16;

    int prevDetected = config.detectedModules;
    if (prevDetected < 0) prevDetected = 0;
    if (count > prevDetected) {
        config.detectedModules = (uint16_t)((count > maxModules) ? maxModules : count);
    } else if (count < prevDetected) {
        Log(LOG_WARN,
            "PWR parser (STACK): transient module drop " +
            String(count) + " < detected " + String(prevDetected) +
            " (keeping detectedModules)");
    }

    int detected = (count > prevDetected) ? count : prevDetected;
    if (detected > maxModules) detected = maxModules;
    if (detected < 1)          detected = 1;

    stackOut.batteryCount = detected;

    long sumVolt = 0;
    long sumCurr = 0;
    long sumSoc  = 0;
    int  socCount = 0;   // modules that actually provided a SOC value
    int maxTemp = -999;

    for (auto& m : modulesOut) {
        sumVolt += m.voltage_mV;
        sumCurr += m.current_mA;
        if (m.fields.count("SOC") || m.fields.count("Coulomb")) {
            sumSoc += m.soc;
            socCount++;
        }
        if (m.temperature > maxTemp) maxTemp = m.temperature;
    }

    stackOut.avgVoltage_mV   = sumVolt / count;
    stackOut.totalCurrent_mA = sumCurr;
    stackOut.temperature     = maxTemp;

    // SOC is the average over the detected modules, but only updated when a SOC
    // value is available for every detected module. If fewer SOC values are
    // present (e.g. a transient drop to 7/8), keep the previous stack SOC so the
    // published/dashboard value stays consistent with the module count.
    if (socCount >= detected && socCount > 0) {
        stackOut.soc = (int)((sumSoc + (socCount / 2)) / socCount);
    } else {
        stackOut.soc = lastParsedStack.soc;
        Log(LOG_WARN,
            "PWR parser (STACK): only " + String(socCount) +
            " SOC value(s) for " + String(detected) +
            " detected module(s) (keeping previous SOC " +
            String(lastParsedStack.soc) + ")");
    }

    config.lastPwrUpdate = config.getCurrentTimeString();

    // Web UI data
    lastParsedStack   = stackOut;
    lastParsedModules = modulesOut;

    // Health evaluation (unchanged, uses modulesOut)
    if (g_healthMutex && xSemaphoreTake(g_healthMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        health.modules.clear();
        health.okModules.clear();
        health.warnModules.clear();
        health.errorModules.clear();

        health.stackCellMin = 0;
        health.stackCellMax = 0;
        health.stackCellDiff = 0;

        auto pushLimited = [&](std::vector<int> &list, int value) {
            if (!list.empty() && list.back() == value)
                return;
            list.push_back(value);
            const size_t MAX_HISTORY = 50;
            if (list.size() > MAX_HISTORY)
                list.erase(list.begin());
        };

        for (auto &m : modulesOut) {
            if (!m.present) continue;

            ModuleHealth mh;
            mh.index = m.index;

            int t1 = m.temperature;
            int t2 = m.fields["Thigh"].toInt();

            if (t1 > 0) mh.tempMax = t1 / 1000.0f;
            if (t2 > 0 && t2 > t1) mh.tempMax = t2 / 1000.0f;

            String vlow  = m.fields["Vlow"];
            String vhigh = m.fields["Vhigh"];

            if (vlow != "-" && vhigh != "-") {
                int low  = vlow.toInt();
                int high = vhigh.toInt();

                if (low > 1000 && low < 5000 && high > 1000 && high < 5000) {
                    mh.cellMin = low / 1000.0f;
                    mh.cellMax = high / 1000.0f;
                    mh.cellDiff = mh.cellMax - mh.cellMin;

                    if (health.stackCellMin == 0 || mh.cellMin < health.stackCellMin)
                        health.stackCellMin = mh.cellMin;

                    if (mh.cellMax > health.stackCellMax)
                        health.stackCellMax = mh.cellMax;
                }
            }

            auto bad = [&](const String &s) {
                if (s.length() == 0) return false;
                if (s == "-") return false;
                if (s == "Normal" || s == "normal") return false;
                return true;
            };

            bool modError = false;
            bool modWarn  = false;

            String volt = m.fields["Volt.St"];
            String curr = m.fields["Curr.St"];
            String temp = m.fields["Temp.St"];
            String sys  = m.fields["SysAlarm.St"];

            if (bad(sys)) modError = true;

            if (bad(volt)) modWarn = true;
            if (bad(curr)) modWarn = true;
            if (bad(temp)) modWarn = true;

            if (mh.cellDiff > config.battery.cellDiffWarn)
                modWarn = true;

            if (mh.cellDiff > config.battery.cellDiffError)
                modError = true;

            if (modError) {
                mh.status = "Fehler";
                health.errorModules.push_back(m.index);
                pushLimited(health.errorHistory, m.index);
            }
            else if (modWarn) {
                mh.status = "Warnung";
                health.warnModules.push_back(m.index);
                pushLimited(health.warnHistory, m.index);
            }
            else {
                mh.status = "OK";
                health.okModules.push_back(m.index);
            }

            health.modules.push_back(mh);
        }

        health.stackCellDiff = health.stackCellMax - health.stackCellMin;

        if (!health.errorModules.empty()) {
            health.color = "red";
            health.strongestMessage = "Fehler in Modulen";
        }
        else if (!health.warnModules.empty()) {
            health.color = "yellow";
            health.strongestMessage = "Warnung in Modulen";
        }
        else {
            health.color = "green";
            health.strongestMessage = "OK";
        }

        xSemaphoreGive(g_healthMutex);
    }

    Log(LOG_INFO, "PWR parser (STACK): parsed " + String(count) + " modules");
    return PARSE_OK;
}

// ---------------------------------------------------------
// HUB MODE PARSER (Hub + Masterhub)
// ---------------------------------------------------------
static ParseResult parsePwrFrameHubMode(const String& raw,
                                        BatteryStack& stackOut,
                                        std::vector<BatteryModule>& modulesOut)
{
    String frame;
    if (!extractFrame(raw, frame)) {
        Log(LOG_WARN, "PWR parser (HUB): no valid @ ... $$ frame found");
        return PARSE_FAIL;
    }

    std::vector<String> lines;
    std::vector<String> header;
    if (!splitFrameLinesAndHeader(frame, lines, header)) {
        return PARSE_FAIL;
    }

    int colAddress = -1;
    int colStack   = -1;
    int colModule  = -1;
    int timeIndex  = -1;

    for (size_t h = 0; h < header.size(); h++) {
        if (header[h] == "Address") colAddress = h;
        if (header[h] == "Stack")   colStack   = h;
        if (header[h] == "Module")  colModule  = h;
        if (header[h] == "Time")    timeIndex  = h;
    }

    if (colStack < 0 || colModule < 0) {
        Log(LOG_WARN, "PWR parser (HUB): missing Stack/Module columns");
        return PARSE_FAIL;
    }

    // hubID -> stackID -> modules
    std::map<int, std::map<int, std::vector<BatteryModule>>> hubMap;

    for (size_t i = 1; i < lines.size(); i++) {

        std::vector<String> cols = splitWS(lines[i]);
        if (cols.empty()) continue;

        // Merge date + time if split
        if (timeIndex >= 0 && cols.size() > (size_t)timeIndex + 1) {
            String datePart = cols[timeIndex];
            String timePart = cols[timeIndex + 1];

            bool looksLikeDate = (datePart.indexOf('-') > 0 || datePart.indexOf('/') > 0);
            bool looksLikeTime = (timePart.indexOf(':') > 0);

            if (looksLikeDate && looksLikeTime) {
                cols[timeIndex] = datePart + " " + timePart;
                cols.erase(cols.begin() + timeIndex + 1);
            }
        }

        if (cols.size() < header.size()) continue;

        if (lastParserValues.empty()) {
            lastParserValues = cols;
        }

        int hubID    = (colAddress >= 0) ? cols[colAddress].toInt() : 1;
        int stackID  = cols[colStack].toInt();
        int moduleID = cols[colModule].toInt();

        BatteryModule mod;
        mod.present = true;
        mod.hub     = hubID;
        mod.stack   = stackID;
        mod.index   = moduleID;

        mod.fields.reserve(header.size());
        for (size_t c = 0; c < header.size(); c++) {
            const String& col   = header[c];
            const String& value = cols[c];

            mod.fields[col] = value;

            if (col == "Volt") {
                mod.voltage_mV = value.toInt();
            }
            else if (col == "Curr") {
                mod.current_mA = value.toInt();
            }
            else if (col == "Tempr") {
                mod.temperature = value.toInt();
            }
            else if (col == "Coulomb") {
                String v = extractLeadingNumber(value);
                mod.fields[col] = v;
                // Coulomb is used as SOC source on some firmware variants.
                mod.soc = normalizeSocPercent(v);
            }
            else if (col == "SOC") {
                mod.soc = normalizeSocPercent(value);
                mod.fields[col] = String(mod.soc);
            }
        }

        bool plausible = true;
        plausible &= (stackID  >= 1 && stackID  <= 5);
        plausible &= (moduleID >= 1 && moduleID <= 16);
        plausible &= (mod.voltage_mV > 10000 && mod.voltage_mV < 60000);

        if (!plausible) {
            Log(LOG_WARN, "PWR parser (HUB): skipping implausible module line " + String(i));
            continue;
        }

        // Keep module row even when optional fields are missing/noisy.
        if (mod.temperature < 0 || mod.temperature > 60000) mod.temperature = 0;
        if (mod.soc < 0) mod.soc = 0;
        if (mod.soc > 100) mod.soc = 100;

        hubMap[hubID][stackID].push_back(mod);
        modulesOut.push_back(mod);
    }

    if (modulesOut.empty()) {
        Log(LOG_WARN, "PWR parser (HUB): no modules parsed");
        return PARSE_FAIL;
    }

    // Sort modules by hub, stack, module
    std::sort(modulesOut.begin(), modulesOut.end(),
        [](const BatteryModule& a, const BatteryModule& b) {
            if (a.hub   != b.hub)   return a.hub   < b.hub;
            if (a.stack != b.stack) return a.stack < b.stack;
            return a.index < b.index;
        });

    // Build per-stack BatteryStack and overall stackOut
    lastParsedHub.hubs.clear();
    BatteryStack total;
    total.reset();

    int totalModules = 0;

    for (auto &hubPair : hubMap) {
        int hubID = hubPair.first;
        for (auto &stackPair : hubPair.second) {
            int stackID = stackPair.first;
            auto &mods  = stackPair.second;

            if (mods.empty()) continue;

            BatteryStack s;
            s.reset();
            s.stackID = stackID;
            s.batteryCount = mods.size();

            long sumVolt = 0;
            long sumCurr = 0;
            long sumSoc  = 0;
            int maxTemp  = -999;

            for (auto &m : mods) {
                sumVolt += m.voltage_mV;
                sumCurr += m.current_mA;
                sumSoc += m.soc;
                if (m.temperature > maxTemp) maxTemp = m.temperature;
            }

            s.avgVoltage_mV   = sumVolt / mods.size();
            s.totalCurrent_mA = sumCurr;
            s.soc             = (int)((sumSoc + ((long)mods.size() / 2)) / (long)mods.size());
            s.temperature     = maxTemp;

            lastParsedHub.hubs[hubID][stackID] = s;

            // Accumulate into total
            totalModules += s.batteryCount;
            total.batteryCount   += s.batteryCount;
            total.avgVoltage_mV  += s.avgVoltage_mV;
            total.totalCurrent_mA += s.totalCurrent_mA;

            if (total.temperature == 0 || s.temperature > total.temperature)
                total.temperature = s.temperature;

            total.soc += (s.soc * s.batteryCount);
        }
    }

    if (totalModules > 0) {
        int stackCount = 0;
        for (auto &hubPair : lastParsedHub.hubs)
            stackCount += hubPair.second.size();

        if (stackCount > 0)
            total.avgVoltage_mV /= stackCount;

        total.soc = (total.soc + (totalModules / 2)) / totalModules;
    }

    stackOut = total;

    int prevDetected = config.detectedModules;
    if (prevDetected < 0) prevDetected = 0;
    if (totalModules > prevDetected) {
        config.detectedModules = totalModules;
    } else {
        // Keep high-water mark to avoid transient module dropouts collapsing 1..N targeting.
        if (totalModules < prevDetected) {
            Log(LOG_WARN,
                "PWR parser (HUB): transient module drop " +
                String(totalModules) + " < detected " + String(prevDetected) +
                " (keeping detectedModules)");
        }
    }
    config.lastPwrUpdate = config.getCurrentTimeString();

    // Web UI data
    lastParsedStack   = stackOut;
    lastParsedModules = modulesOut;

    // Health evaluation (reuse same logic, now across all modules)
    if (g_healthMutex && xSemaphoreTake(g_healthMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        health.modules.clear();
        health.okModules.clear();
        health.warnModules.clear();
        health.errorModules.clear();

        health.stackCellMin = 0;
        health.stackCellMax = 0;
        health.stackCellDiff = 0;

        auto pushLimited = [&](std::vector<int> &list, int value) {
            if (!list.empty() && list.back() == value)
                return;
            list.push_back(value);
            const size_t MAX_HISTORY = 50;
            if (list.size() > MAX_HISTORY)
                list.erase(list.begin());
        };

        for (auto &m : modulesOut) {
            if (!m.present) continue;

            ModuleHealth mh;
            mh.index = m.index;

            int t1 = m.temperature;
            int t2 = m.fields["Thigh"].toInt();

            if (t1 > 0) mh.tempMax = t1 / 1000.0f;
            if (t2 > 0 && t2 > t1) mh.tempMax = t2 / 1000.0f;

            String vlow  = m.fields["Vlow"];
            String vhigh = m.fields["Vhigh"];

            if (vlow != "-" && vhigh != "-") {
                int low  = vlow.toInt();
                int high = vhigh.toInt();

                if (low > 1000 && low < 5000 && high > 1000 && high < 5000) {
                    mh.cellMin = low / 1000.0f;
                    mh.cellMax = high / 1000.0f;
                    mh.cellDiff = mh.cellMax - mh.cellMin;

                    if (health.stackCellMin == 0 || mh.cellMin < health.stackCellMin)
                        health.stackCellMin = mh.cellMin;

                    if (mh.cellMax > health.stackCellMax)
                        health.stackCellMax = mh.cellMax;
                }
            }

            auto bad = [&](const String &s) {
                if (s.length() == 0) return false;
                if (s == "-") return false;
                if (s == "Normal" || s == "normal") return false;
                return true;
            };

            bool modError = false;
            bool modWarn  = false;

            String volt = m.fields["Volt.St"];
            String curr = m.fields["Curr.St"];
            String temp = m.fields["Temp.St"];
            String sys  = m.fields["SysAlarm.St"];

            if (bad(sys)) modError = true;

            if (bad(volt)) modWarn = true;
            if (bad(curr)) modWarn = true;
            if (bad(temp)) modWarn = true;

            if (mh.cellDiff > config.battery.cellDiffWarn)
                modWarn = true;

            if (mh.cellDiff > config.battery.cellDiffError)
                modError = true;

            if (modError) {
                mh.status = "Fehler";
                health.errorModules.push_back(m.index);
                pushLimited(health.errorHistory, m.index);
            }
            else if (modWarn) {
                mh.status = "Warnung";
                health.warnModules.push_back(m.index);
                pushLimited(health.warnHistory, m.index);
            }
            else {
                mh.status = "OK";
                health.okModules.push_back(m.index);
            }

            health.modules.push_back(mh);
        }

        health.stackCellDiff = health.stackCellMax - health.stackCellMin;

        if (!health.errorModules.empty()) {
            health.color = "red";
            health.strongestMessage = "Fehler in Modulen";
        }
        else if (!health.warnModules.empty()) {
            health.color = "yellow";
            health.strongestMessage = "Warnung in Modulen";
        }
        else {
            health.color = "green";
            health.strongestMessage = "OK";
        }

        xSemaphoreGive(g_healthMutex);
    }

    Log(LOG_INFO, "PWR parser (HUB): parsed " + String(totalModules) + " modules");
    return PARSE_OK;
}
