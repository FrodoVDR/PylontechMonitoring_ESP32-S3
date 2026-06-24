#include "py_parser_bat.h"
#include "py_log.h"
#include "py_uart.h"
#include "config.h"
#include <map>

extern PyUart py_uart;

// Globale BAT-Daten (für Web-UI)
std::vector<BatData> lastParsedBatCells;
BatData lastParsedBat;

// Safety-first: disabled by default, can be enabled for targeted BAT robustness tests.
#ifndef BAT_ENABLE_PARTIAL_RECOVERY
#define BAT_ENABLE_PARTIAL_RECOVERY 0
#endif
static constexpr bool kEnableBatPartialRecovery = (BAT_ENABLE_PARTIAL_RECOVERY != 0);

// ---------------------------------------------------------
// Helper: trim whitespace
// ---------------------------------------------------------
static String trimWS(const String& s) {
    String r = s;
    r.trim();
    return r;
}

// Keep only the numeric prefix if a value contains a trailing unit, e.g. "61585 mAH" -> "61585".
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

    // Accept numeric prefix if remaining chars are just a unit/suffix.
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
// Extract @ ... $$ frame
// ---------------------------------------------------------
static bool extractFrame(const String& raw, String& frame) {
    int start = raw.indexOf('@');
    if (start < 0) return false;
    // Search the terminator strictly after '@' so a '$$' that appears in a
    // preceding asynchronous log line cannot mis-bound the frame.
    int end = raw.indexOf("$$", start + 1);

    if (end < 0 || end <= start)
        return false;

    frame = raw.substring(start + 1, end);
    return true;
}

// ---------------------------------------------------------
// Split columns by 2+ spaces
// ---------------------------------------------------------
static std::vector<String> splitColumns(const String& line) {
    std::vector<String> out;
    int len = line.length();
    int start = 0;

    while (start < len) {
        while (start < len && line[start] == ' ') start++;
        if (start >= len) break;

        int end = start;
        int spaceCount = 0;

        while (end < len) {
            char c = line[end];
            if (c == ' ') {
                spaceCount++;
                if (spaceCount >= 2) break;
            } else {
                spaceCount = 0;
            }
            end++;
        }

        String col = line.substring(start, end);
        col.trim();
        if (col.length() > 0) out.push_back(col);

        start = end;
    }

    return out;
}

static String cellKey(const BatData& cell) {
    if (!cell.fields.empty()) {
        String k = cell.fields[0].raw;
        k.trim();
        if (k.length() > 0) return k;
    }
    return String(cell.cellIndex);
}

static bool recoverBatPartialFrame(
    int moduleIdx,
    const std::vector<BatData>& parsedCells,
    size_t expectedCells,
    std::vector<BatData>& recoveredOut)
{
    if (!kEnableBatPartialRecovery) return false;
    if (parsedCells.size() + 1 != expectedCells) return false;
    if (lastParsedBatCells.size() != expectedCells) return false;

    for (const auto& prevCell : lastParsedBatCells) {
        if (prevCell.moduleIndex != moduleIdx) return false;
    }

    std::map<String, size_t> prevIdxByKey;
    for (size_t i = 0; i < lastParsedBatCells.size(); ++i) {
        prevIdxByKey[cellKey(lastParsedBatCells[i])] = i;
    }

    recoveredOut = lastParsedBatCells;
    size_t replaced = 0;

    for (const auto& curCell : parsedCells) {
        auto it = prevIdxByKey.find(cellKey(curCell));
        if (it != prevIdxByKey.end()) {
            recoveredOut[it->second] = curCell;
            replaced++;
        }
    }

    // Only accept if almost all current rows could be matched by key.
    return (replaced >= parsedCells.size() - 1);
}

// ---------------------------------------------------------
// Main BAT parser
// ---------------------------------------------------------
ParseResult parseBatFrame(int /*moduleIndex*/,
                          const String& raw,
                          BatData& out)
{
    static constexpr size_t kExpectedBatCells = 15;

    std::vector<BatData> parsedCells;
    parsedCells.reserve(kExpectedBatCells);
    out.fields.clear();
    out.cellIndex = -1;

    // ---------------------------------------------------------
    // 1) Check if this frame belongs to BAT
    // ---------------------------------------------------------
    String last = py_uart.getLastCommand();
    String lastLower = last;
    lastLower.trim();
    lastLower.toLowerCase();

    if (!lastLower.startsWith("bat")) {
        Log(LOG_DEBUG, "BAT parser: ignoring frame (last command was '" + last + "')");
        return PARSE_IGNORED;
    }

    // Extract module index from command (bat1, bat2, ...)
    String num = lastLower.substring(3);
    num.trim();
    int moduleIdx = num.toInt();

    if (moduleIdx < 1 || moduleIdx > 16) {
        Log(LOG_WARN, "BAT parser: invalid module index '" + last + "'");
        return PARSE_IGNORED;
    }

    out.moduleIndex = moduleIdx;

    // ---------------------------------------------------------
    // 2) Validate frame
    // ---------------------------------------------------------
    if (!py_uart.isFrameValid()) {
        Log(LOG_WARN, "BAT parser: skipping invalid frame");
        return PARSE_FAIL;
    }

    Log(LOG_INFO, "BAT parser: raw frame received for module " + String(moduleIdx));

    // ---------------------------------------------------------
    // 3) Extract @ ... $$ section
    // ---------------------------------------------------------
    String frame;
    if (!extractFrame(raw, frame)) {
        Log(LOG_WARN, "BAT parser: no valid @ ... $$ frame found");
        return PARSE_FAIL;
    }

    frame.replace("\r\n", "\n");
    frame.replace("\r", "\n");

    // ---------------------------------------------------------
    // 4) Split into lines
    // ---------------------------------------------------------
    std::vector<String> lines;
    {
        int pos = 0;
        while (true) {
            int nl = frame.indexOf('\n', pos);
            if (nl < 0) {
                String lastLine = trimWS(frame.substring(pos));
                if (lastLine.length() > 0) lines.push_back(lastLine);
                break;
            }
            String line = trimWS(frame.substring(pos, nl));
            if (line.length() > 0) lines.push_back(line);
            pos = nl + 1;
        }
    }

    if (lines.size() < 2) {
        Log(LOG_WARN, "BAT parser: too few lines");
        return PARSE_FAIL;
    }

    // ---------------------------------------------------------
    // 5) Header
    // ---------------------------------------------------------
    std::vector<String> header = splitColumns(lines[0]);
    if (header.empty()) {
        Log(LOG_WARN, "BAT parser: empty header");
        return PARSE_FAIL;
    }

    // ---------------------------------------------------------
    // 6) Parse cell rows
    // ---------------------------------------------------------
    for (int row = 1; row < (int)lines.size(); row++) {
        String line = lines[row];
        String lineLower = line;
        lineLower.toLowerCase();
        if (lineLower.startsWith("command completed")) {
            break;
        }

        // Stop at non-numeric first token
        String firstToken = line;
        firstToken.trim();
        int spacePos = firstToken.indexOf(' ');
        if (spacePos > 0)
            firstToken = firstToken.substring(0, spacePos);

        if (firstToken.length() == 0)
            continue;

        // A genuine BAT cell row starts with a pure integer cell index. Pylontech
        // asynchronous event/log lines can be interleaved into the frame body
        // (timestamps, hex codes, text). Reject them instead of treating them as
        // cells, which would otherwise corrupt the data or break the column count.
        bool firstTokenIsInt = true;
        for (int i = 0; i < (int)firstToken.length(); ++i) {
            if (!isDigit(firstToken[i])) { firstTokenIsInt = false; break; }
        }
        if (!firstTokenIsInt) {
            // Ignore occasional noise/log lines instead of terminating the parse early.
            continue;
        }

        std::vector<String> cols = splitColumns(line);
        if (cols.empty()) continue;
        // Real cell rows are wide (index + voltage + current + temperature + ...).
        // A short numeric-leading line is log noise, not a cell row.
        if (cols.size() < 4) continue;

        size_t count = min(header.size(), cols.size());

        BatData cell;
        cell.cellIndex = (int)parsedCells.size();
        cell.moduleIndex = moduleIdx;

        for (size_t c = 0; c < count; c++) {
            BatField f;
            f.name = header[c];

            String value = cols[c];
            String nameLower = header[c];
            nameLower.toLowerCase();

            // Some firmwares append unit text to Coulomb/SOC values (e.g. "mAH" or "%").
            // Keep Coulomb numeric and force SOC into 0..100.
            if (nameLower == "soc") {
                value = String(normalizeSocPercent(value));
            }
            else if (nameLower.indexOf("coulomb") >= 0) {
                value = extractLeadingNumber(value);
            }

            f.raw  = value;
            cell.fields.push_back(f);
        }

        parsedCells.push_back(cell);
    }

    if (parsedCells.size() < kExpectedBatCells) {
        std::vector<BatData> recoveredCells;
        if (recoverBatPartialFrame(moduleIdx, parsedCells, kExpectedBatCells, recoveredCells)) {
            lastParsedBatCells = recoveredCells;

            if (!lastParsedBatCells.empty()) {
                out = lastParsedBatCells[0];
                if (g_batMutex && xSemaphoreTake(g_batMutex, portMAX_DELAY) == pdTRUE) {
                    lastParsedBat = out;
                    xSemaphoreGive(g_batMutex);
                }
            }

            Log(LOG_WARN,
                "BAT parser: recovered partial frame for module " + String(moduleIdx) +
                " (cells=" + String((int)parsedCells.size()) + "/" + String((int)kExpectedBatCells) + ")");
            return PARSE_OK;
        }

        Log(LOG_WARN,
            "BAT parser: incomplete frame for module " + String(moduleIdx) +
            " (cells=" + String((int)parsedCells.size()) + "/" + String((int)kExpectedBatCells) + ")");
        return PARSE_FAIL;
    }

    // Commit only complete/valid BAT frames so partial reads never replace good data.
    lastParsedBatCells = parsedCells;

    // ---------------------------------------------------------
    // 7) Store first cell for convenience – mutex-geschützt
    // ---------------------------------------------------------
    if (!lastParsedBatCells.empty()) {
        out = lastParsedBatCells[0];
        if (g_batMutex && xSemaphoreTake(g_batMutex, portMAX_DELAY) == pdTRUE) {
            lastParsedBat = out;
            xSemaphoreGive(g_batMutex);
        }
    }

    Log(LOG_INFO, "BAT parser: parsed " + String(lastParsedBatCells.size()) +
                  " cells for module " + String(moduleIdx));

    return PARSE_OK;
}
