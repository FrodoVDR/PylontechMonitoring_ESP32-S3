#include "py_parser_info.h"
#include "py_log.h"
#include "py_uart.h"
#include <set>

extern PyUart py_uart;

// Global INFO storage (for Web UI)
InfoData lastParsedInfo;
String infoLastStatusMessage = "Noch kein INFO-Frame geparst";
bool infoLastParseOk = false;
unsigned long infoLastStatusMillis = 0;

// ---------------------------------------------------------
// Helper: trim whitespace
// ---------------------------------------------------------
static String infoTrimWS(const String& s) {
    String r = s;
    r.trim();
    return r;
}

static bool infoHasAlpha(const String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    }
    return false;
}

static bool infoKeyLooksPlausible(const String& key) {
    if (key.length() < 2 || key.length() > 48) return false;

    // INFO field names should start with a letter, not with timestamps/counters.
    char c0 = key[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;

    // Drop obvious malformed lines that accidentally became keys.
    // Note: double spaces are allowed – some INFO fields use them (e.g. "Soft  version").
    if (key.indexOf("$") >= 0 || key.indexOf("@") >= 0) return false;

    return true;
}

static bool infoDataLooksConsistent(const InfoData& out) {
    if (out.fields.size() < 6) return false;

    std::set<String> seen;
    for (const auto& f : out.fields) {
        if (!infoKeyLooksPlausible(f.name)) return false;
        // Some firmware variants return a few empty values in INFO - allow those.
        if (seen.count(f.name)) return false;
        seen.insert(f.name);
    }

    // Different Pylontech firmware variants expose different INFO keys.
    // Accept any plausible, non-duplicate key set with a minimum field count.
    return true;
}

// ---------------------------------------------------------
// Extract @ ... $$ frame
// ---------------------------------------------------------
static bool infoExtractFrame(const String& raw, String& frame) {
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
// INFO parser
// ---------------------------------------------------------
ParseResult parseInfoFrame(int /*moduleIndex*/,
                           const String& raw,
                           InfoData& out)
{
    out.fields.clear();
    // Reserve up front to avoid reallocation churn / fragmentation.
    out.fields.reserve(28);
    out.moduleIndex = -1;

    // ---------------------------------------------------------
    // 1) Check if this frame belongs to INFO
    // ---------------------------------------------------------
    String last = py_uart.getLastCommand();
    String lastLower = last;
    lastLower.trim();
    lastLower.toLowerCase();

    if (!lastLower.startsWith("info")) {
        Log(LOG_DEBUG, "INFO parser: ignoring frame (last command was '" + last + "')");
        infoLastStatusMessage = "Frame ignoriert: letzter Befehl war nicht INFO";
        infoLastParseOk = false;
        infoLastStatusMillis = millis();
        return PARSE_IGNORED;
    }

    // Extract module index
    int idx = lastLower.substring(4).toInt();
    if (idx <= 0 || idx > 16) {
        Log(LOG_WARN, "INFO parser: invalid module index in command '" + last + "'");
        infoLastStatusMessage = "Ungueltiger INFO-Modulindex in Kommando";
        infoLastParseOk = false;
        infoLastStatusMillis = millis();
        return PARSE_IGNORED;
    }

    out.moduleIndex = idx;

    // ---------------------------------------------------------
    // 2) Validate frame
    // ---------------------------------------------------------
    if (!py_uart.isFrameValid()) {
        Log(LOG_WARN, "INFO parser: frame marked invalid, trying anyway");
    }

    Log(LOG_INFO, "INFO parser: raw frame received for module " + String(idx));

    // ---------------------------------------------------------
    // 3) Extract @ ... $$ section
    // ---------------------------------------------------------
    String frame;
    if (!infoExtractFrame(raw, frame)) {
        Log(LOG_WARN, "INFO parser: no valid @ ... $$ frame found");
        infoLastStatusMessage = "INFO-Frame unvollstaendig (@...$$ fehlt)";
        infoLastParseOk = false;
        infoLastStatusMillis = millis();
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
            line = infoTrimWS(frame.substring(pos));
            pos = frame.length();
        } else {
            line = infoTrimWS(frame.substring(pos, nl));
            pos = nl + 1;
        }

        if (line.length() == 0)
            continue;

        // End of output
        if (line.startsWith("Command completed"))
            break;

        // Key : Value (robust)
        int colon = line.indexOf(" : ");
        int sepLen = 3;
        if (colon < 0) {
            colon = line.indexOf(':');
            sepLen = 1;
        }
        String key, value;

        if (colon >= 0) {
            key   = infoTrimWS(line.substring(0, colon));
            value = infoTrimWS(line.substring(colon + sepLen));
        }
        else {
            // Fallback: last token is value (only if key looks like a real field name)
            int split = line.lastIndexOf(' ');
            if (split > 0) {
                key   = infoTrimWS(line.substring(0, split));
                value = infoTrimWS(line.substring(split + 1));
            }
            else {
                continue;
            }
        }

        if (key.length() == 0)
            continue;

        // Ignore banner/noise lines that do not look like INFO fields
        if (!infoHasAlpha(key))
            continue;

        if (!infoKeyLooksPlausible(key))
            continue;

        if (value.length() == 0)
            continue;

        InfoField f;
        f.name = key;
        f.raw  = value;
        out.fields.push_back(f);
    }

    if (safetyCounter >= 1000) {
        Log(LOG_ERROR, "INFO parser: safety break triggered (malformed frame)");
        infoLastStatusMessage = "INFO-Frame fehlerhaft (safety break)";
        infoLastParseOk = false;
        infoLastStatusMillis = millis();
        return PARSE_FAIL;
    }

    if (!infoDataLooksConsistent(out)) {
        Log(LOG_WARN, "INFO parser: consistency check failed, frame rejected");
        infoLastStatusMessage = "INFO-Frame inkonsistent und wurde verworfen";
        infoLastParseOk = false;
        infoLastStatusMillis = millis();
        return PARSE_FAIL;
    }

    // ---------------------------------------------------------
    // 5) Store global result (for Web UI) – mutex-protected
    // Keep the richest dataset across module sweep to avoid temporary
    // incomplete INFO tables when a later module has fewer fields.
    // ---------------------------------------------------------
    if (g_infoMutex && xSemaphoreTake(g_infoMutex, portMAX_DELAY) == pdTRUE) {
        bool replace = lastParsedInfo.fields.empty()
                    || (out.fields.size() >= lastParsedInfo.fields.size());
        if (replace) {
            lastParsedInfo = out;
        }
        xSemaphoreGive(g_infoMutex);
    }

    InfoBuffer* target = infoUseA ? &infoB : &infoA;
    target->info = out;
    infoUseA = !infoUseA;

    Log(LOG_INFO, "INFO parser: parsed " + String(out.fields.size()) +
                  " fields for module " + String(idx));

    infoLastStatusMessage = "INFO OK: " + String(out.fields.size()) + " Felder (Modul " + String(idx) + ")";
    infoLastParseOk = true;
    infoLastStatusMillis = millis();

    return PARSE_OK;
}
