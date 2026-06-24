#include "py_parser_stat.h"
#include "py_log.h"
#include "py_uart.h"

extern PyUart py_uart;

// Global STAT storage (for Web UI)
StatData lastParsedStat;

// ---------------------------------------------------------
// Helper: trim whitespace
// ---------------------------------------------------------
static String trimWS(const String& s) {
    String r = s;
    r.trim();
    return r;
}

static bool statHasAlpha(const String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    }
    return false;
}

static bool statKeyLooksPlausible(const String& key) {
    if (key.length() < 2 || key.length() > 64) return false;

    char c0 = key[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;

    if (key.indexOf('$') >= 0 || key.indexOf('@') >= 0) return false;

    return true;
}

static bool statLooksLikeTelemetryRow(const String& line) {
    int i = 0;
    while (i < (int)line.length() && line[i] == ' ') i++;

    int digits = 0;
    while (i < (int)line.length() && line[i] >= '0' && line[i] <= '9') {
        digits++;
        i++;
    }

    if (digits < 5) return false;

    while (i < (int)line.length() && line[i] == ' ') i++;

    if (i + 8 >= (int)line.length()) return false;
    if (!(line[i + 0] >= '0' && line[i + 0] <= '9')) return false;
    if (!(line[i + 1] >= '0' && line[i + 1] <= '9')) return false;
    if (line[i + 2] != '-') return false;
    if (!(line[i + 3] >= '0' && line[i + 3] <= '9')) return false;
    if (!(line[i + 4] >= '0' && line[i + 4] <= '9')) return false;
    if (line[i + 5] != '-') return false;
    if (!(line[i + 6] >= '0' && line[i + 6] <= '9')) return false;
    if (!(line[i + 7] >= '0' && line[i + 7] <= '9')) return false;

    return true;
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
// STAT parser
// ---------------------------------------------------------
ParseResult parseStatFrame(int /*moduleIndex*/,
                           const String& raw,
                           StatData& out)
{
    out.fields.clear();
    // Reserve up front to avoid geometric reallocations (and the
    // resulting heap fragmentation) while pushing ~100 STAT fields.
    out.fields.reserve(110);
    out.moduleIndex = -1;

    // ---------------------------------------------------------
    // 1) Check if this frame belongs to STAT
    // ---------------------------------------------------------
    String last = py_uart.getLastCommand();
    String lastLower = last;
    lastLower.trim();
    lastLower.toLowerCase();

    if (!lastLower.startsWith("stat")) {
        Log(LOG_DEBUG, "STAT parser: ignoring frame (last command was '" + last + "')");
        return PARSE_IGNORED;
    }

    // Extract module index
    int idx = lastLower.substring(4).toInt();
    if (idx <= 0 || idx > 16) {
        Log(LOG_WARN, "STAT parser: invalid module index in command '" + last + "'");
        return PARSE_IGNORED;
    }

    out.moduleIndex = idx;

    // ---------------------------------------------------------
    // 2) Validate frame
    // ---------------------------------------------------------
    if (!py_uart.isFrameValid()) {
        Log(LOG_WARN, "STAT parser: frame marked invalid, trying anyway");
    }

    Log(LOG_INFO, "STAT parser: raw frame received for module " + String(idx));

    // ---------------------------------------------------------
    // 3) Extract @ ... $$ section
    // ---------------------------------------------------------
    String frame;
    if (!extractFrame(raw, frame)) {
        Log(LOG_WARN, "STAT parser: no valid @ ... $$ frame found");
        return PARSE_FAIL;
    }

    frame.replace("\r\n", "\n");
    frame.replace("\r", "\n");

    // ---------------------------------------------------------
    // 4) Parse lines robustly
    // ---------------------------------------------------------
    int pos = 0;
    int safetyCounter = 0;

    while (pos < frame.length() && safetyCounter < 1000) {

        safetyCounter++;

        int nl = frame.indexOf('\n', pos);
        String line;

        if (nl < 0) {
            line = trimWS(frame.substring(pos));
            pos = frame.length();
        } else {
            line = trimWS(frame.substring(pos, nl));
            pos = nl + 1;
        }

        if (line.length() == 0)
            continue;

        if (statLooksLikeTelemetryRow(line))
            continue;

        // End of output
        if (line.startsWith("Command completed"))
            break;

        // Key : Value
        int colon = line.indexOf(':');
        String key, value;

        if (colon >= 0) {
            key   = trimWS(line.substring(0, colon));
            value = trimWS(line.substring(colon + 1));
        }
        else {
            // Fallback: last token is value
            int split = line.lastIndexOf(' ');
            if (split > 0) {
                key   = trimWS(line.substring(0, split));
                value = trimWS(line.substring(split + 1));
            }
            else {
                continue;
            }
        }

        if (key.length() == 0)
            continue;

        if (!statHasAlpha(key))
            continue;

        if (!statKeyLooksPlausible(key))
            continue;

        if (value.length() == 0)
            continue;

        StatField f;
        f.name = key;
        f.raw  = value;
        out.fields.push_back(f);
    }

    if (safetyCounter >= 1000) {
        Log(LOG_ERROR, "STAT parser: safety break triggered (malformed frame)");
        return PARSE_FAIL;
    }

    // ---------------------------------------------------------
    // 5) Store global result (for Web UI) – mutex-protected
    // ---------------------------------------------------------
    if (g_statMutex && xSemaphoreTake(g_statMutex, portMAX_DELAY) == pdTRUE) {
        lastParsedStat = out;
        xSemaphoreGive(g_statMutex);
    }

    StatBuffer* target = statUseA ? &statB : &statA;
    target->stat = out;
    statUseA = !statUseA;

    Log(LOG_INFO, "STAT parser: parsed " + String(out.fields.size()) +
                  " fields for module " + String(idx));

    return PARSE_OK;
}
