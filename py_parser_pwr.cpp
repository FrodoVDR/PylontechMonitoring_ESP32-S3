#include "py_parser_pwr.h"
#include "py_log.h"
#include "config.h"
#include "py_uart.h"

#include <cctype>
#include <algorithm>
#include <map>

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
    int end   = raw.indexOf("$$");

    if (start < 0 || end < 0 || end <= start)
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
            else if (col == "Coulomb" || col == "SOC") {
                String v = value;
                v.replace("%", "");
                mod.soc = v.toInt();
            }
        }

        bool plausible = true;
        plausible &= (mod.index >= 1 && mod.index <= 32);
        plausible &= (mod.voltage_mV > 10000 && mod.voltage_mV < 60000);
        plausible &= (mod.temperature > 1000 && mod.temperature < 60000);
        plausible &= (mod.soc >= 1 && mod.soc <= 100);

        if (!plausible) {
            Log(LOG_WARN, "PWR parser (STACK): skipping implausible module line " + String(i));
            continue;
        }

        modulesOut.push_back(mod);
    }

    if (modulesOut.empty()) {
        Log(LOG_WARN, "PWR parser (STACK): no modules parsed");
        return PARSE_FAIL;
    }

    // Stack calculation (unchanged logic)
    int count = modulesOut.size();
    stackOut.batteryCount = count;
    config.detectedModules = count;

    long sumVolt = 0;
    long sumCurr = 0;
    int minSoc = 999;
    int maxTemp = -999;

    for (auto& m : modulesOut) {
        sumVolt += m.voltage_mV;
        sumCurr += m.current_mA;

        if (m.soc < minSoc) minSoc = m.soc;
        if (m.temperature > maxTemp) maxTemp = m.temperature;
    }

    stackOut.avgVoltage_mV   = sumVolt / count;
    stackOut.totalCurrent_mA = sumCurr;
    stackOut.soc             = minSoc;
    stackOut.temperature     = maxTemp;

    config.lastPwrUpdate = config.getCurrentTimeString();

    // Web UI data
    lastParsedStack   = stackOut;
    lastParsedModules = modulesOut;

    // Double buffer
    PwrBuffer* target = pwrUseA ? &pwrB : &pwrA;
    target->stack   = stackOut;
    target->modules = modulesOut;
    pwrUseA = !pwrUseA;

    // Health evaluation (unchanged, uses modulesOut)
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
            else if (col == "Coulomb" || col == "SOC") {
                String v = value;
                v.replace("%", "");
                mod.soc = v.toInt();
            }
        }

        bool plausible = true;
        plausible &= (stackID  >= 1 && stackID  <= 5);
        plausible &= (moduleID >= 1 && moduleID <= 16);
        plausible &= (mod.voltage_mV > 10000 && mod.voltage_mV < 60000);
        plausible &= (mod.temperature > 1000 && mod.temperature < 60000);
        plausible &= (mod.soc >= 1 && mod.soc <= 100);

        if (!plausible) {
            Log(LOG_WARN, "PWR parser (HUB): skipping implausible module line " + String(i));
            continue;
        }

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
            int minSoc   = 999;
            int maxTemp  = -999;

            for (auto &m : mods) {
                sumVolt += m.voltage_mV;
                sumCurr += m.current_mA;
                if (m.soc < minSoc) minSoc = m.soc;
                if (m.temperature > maxTemp) maxTemp = m.temperature;
            }

            s.avgVoltage_mV   = sumVolt / mods.size();
            s.totalCurrent_mA = sumCurr;
            s.soc             = minSoc;
            s.temperature     = maxTemp;

            lastParsedHub.hubs[hubID][stackID] = s;

            // Accumulate into total
            totalModules += s.batteryCount;
            total.batteryCount   += s.batteryCount;
            total.avgVoltage_mV  += s.avgVoltage_mV;
            total.totalCurrent_mA += s.totalCurrent_mA;

            if (total.temperature == 0 || s.temperature > total.temperature)
                total.temperature = s.temperature;

            if (total.soc == 0 || (s.soc > 0 && s.soc < total.soc))
                total.soc = s.soc;
        }
    }

    if (totalModules > 0) {
        int stackCount = 0;
        for (auto &hubPair : lastParsedHub.hubs)
            stackCount += hubPair.second.size();

        if (stackCount > 0)
            total.avgVoltage_mV /= stackCount;
    }

    stackOut = total;
    config.detectedModules = totalModules;
    config.lastPwrUpdate = config.getCurrentTimeString();

    // Web UI data
    lastParsedStack   = stackOut;
    lastParsedModules = modulesOut;

    // Double buffer
    PwrBuffer* target = pwrUseA ? &pwrB : &pwrA;
    target->stack   = stackOut;
    target->modules = modulesOut;
    pwrUseA = !pwrUseA;

    // Health evaluation (reuse same logic, now across all modules)
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

    Log(LOG_INFO, "PWR parser (HUB): parsed " + String(totalModules) + " modules");
    return PARSE_OK;
}
