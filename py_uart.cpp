#include "py_uart.h"
#include "py_log.h"
#include "py_parser_pwr.h"
#include "py_parser_bat.h"
#include "py_parser_stat.h"
#include "py_parser_info.h"

#include "config.h"   // enthält PwrBuffer, BatBuffer, StatBuffer + Flags
#include "esp_heap_caps.h"


#define BAT_RX_PIN 16
#define BAT_TX_PIN 17

// UART receive buffer. Allocated in PSRAM to keep ~20 KB out of the scarce
// internal DRAM heap (frees room for MQTT publishing and lwIP). Falls back to
// internal heap only if PSRAM is unavailable. Never freed (lives for the whole
// runtime), so this is effectively a one-time relocation off the internal heap.
static const size_t G_RECV_BUFF_SIZE = 20000;
static char* g_szRecvBuff = nullptr;
static int g_invalidCount = 0;

// Ensure the receive buffer exists. Prefers PSRAM; on failure uses internal
// heap so the UART stays functional even without working SPI RAM.
static char* ensureRecvBuff() {
    if (g_szRecvBuff) return g_szRecvBuff;
    g_szRecvBuff = (char*)heap_caps_malloc(G_RECV_BUFF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_szRecvBuff) {
        g_szRecvBuff = (char*)heap_caps_malloc(G_RECV_BUFF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        Log(LOG_WARN, "UART: recv buffer in internal heap (PSRAM unavailable)");
    } else {
        Log(LOG_INFO, "UART: recv buffer allocated in PSRAM (" + String((uint32_t)G_RECV_BUFF_SIZE) + "B)");
    }
    return g_szRecvBuff;
}

// ---------------------------------------------------------
static bool isValidFrame(const String& f) {
    bool hasAt = (f.indexOf("@") >= 0);
    bool hasDollar = (f.indexOf("$$") >= 0);
    bool hasPylon = (f.indexOf("pylon>") >= 0);
    bool enoughLen = (f.length() >= 40);
    
    int lines = 0;
    for (int i = 0; i < f.length(); i++)
        if (f[i] == '\n') lines++;
    bool enoughLines = (lines >= 3);
    
    bool notBadPress = true;
    if (f.indexOf("Press [Enter]") >= 0 &&
        f.indexOf("Remote command:") < 0) {
        notBadPress = false;
    }
    
    // Treat "pylon>" as optional. Some valid frames end at "$$" and the prompt
    // may arrive late or not at all under load.
    bool result = hasAt && hasDollar && enoughLen && enoughLines && notBadPress;
    
    // Detailed logging for invalid frames
    if (!result) {
        String dbg = "Frame validation failed: ";
        if (!hasAt) dbg += "[no @] ";
        if (!hasDollar) dbg += "[no $$] ";
        if (!hasPylon) dbg += "[no pylon>] ";
        if (!enoughLen) dbg += "[len=" + String(f.length()) + "] ";
        if (!enoughLines) dbg += "[lines=" + String(lines) + "] ";
        if (!notBadPress) dbg += "[badPress] ";
        dbg += " | first 200 chars: " + f.substring(0, 200);
        Log(LOG_WARN, dbg);
    }
    
    return result;
}

bool detectHubMode() {
    unsigned long start = millis();
    const unsigned long timeout = 10000; // 2.5 Sekunden zuhören

    String buffer = "";

    while (millis() - start < timeout) {
        while (Serial2.available()) {
            char c = Serial2.read();
            buffer += c;

            // Heuristik: Hub-PWR-Frame enthält immer '@' und '$$'
            if (buffer.indexOf("@") >= 0 &&
                buffer.indexOf("$$") >= 0 &&
                buffer.length() > 200) {

                Log(LOG_INFO, "UART: Hub-Frame erkannt → HUB-Modus");
                return true;
            }
        }
        delay(5);
    }

    Log(LOG_INFO, "UART: Keine Hub-Daten empfangen → STACK-Modus");
    return false;
}

// ---------------------------------------------------------
// HUB mode: non-blocking passive listener
// Accumulates bytes from Serial2 into hubBuf.
// Returns true (and sets lastRawFrame/frameValid) once a complete
// @ ... $$ frame is found. Older/partial data before '@' is discarded.
// ---------------------------------------------------------
bool PyUart::pollHubFrame() {
    // Drain all currently available bytes
    while (Serial2.available()) {
        hubBuf += (char)Serial2.read();
    }

    // Prevent memory exhaustion: keep only the last 24 KB
    if (hubBuf.length() > 24000) {
        int atPos = hubBuf.lastIndexOf('@');
        hubBuf = (atPos > 0) ? hubBuf.substring(atPos) : "";
        Log(LOG_WARN, "UART HUB: buffer trimmed");
    }

    // Look for a complete @ ... $$ frame
    int atPos = hubBuf.indexOf('@');
    if (atPos < 0) {
        hubBuf = "";   // no frame start – discard garbage
        return false;
    }
    if (atPos > 0)
        hubBuf = hubBuf.substring(atPos);  // trim leading garbage

    int ddPos = hubBuf.indexOf("$$");
    if (ddPos < 0) return false;  // frame not yet complete

    // Extract frame (include the $$ terminator)
    int frameEnd = ddPos + 2;
    String frame = hubBuf.substring(0, frameEnd);
    hubBuf = (frameEnd < (int)hubBuf.length()) ? hubBuf.substring(frameEnd) : "";

    // Minimal plausibility check (HUB frames may not contain "pylon>")
    if (frame.length() < 200) return false;

    lastRawFrame  = frame;
    lastCommand   = "pwr";
    frameReady    = true;
    frameValid    = true;

    Log(LOG_INFO, "UART HUB: passive frame received (" + String(frame.length()) + " bytes)");
    return true;
}

// ---------------------------------------------------------
void PyUart::begin(int rx, int tx) {
    rxPin = rx;
    txPin = tx;

    Serial2.setRxBufferSize(4096);  // long pwr/bat listings overflow the default 256B FIFO -> dropped rows
    Serial2.begin(115200, SERIAL_8N1, rxPin, txPin);
    delay(50);

    ensureRecvBuff();
    lastRawFrame.reserve(4096);  // stable capacity -> assignment reuses buffer, no realloc churn
    if (!consoleMutex) consoleMutex = xSemaphoreCreateMutex();

    Log(LOG_INFO, "UART: begin() RX=" + String(rxPin) + " TX=" + String(txPin));

    // ---------------------------------------------------------
    // AUTO-DETECTION: HUB oder STACK
    // ---------------------------------------------------------
    if (g_batteryMode == BatteryMode::UNKNOWN) {
        Log(LOG_INFO, "UART: Auto-Erkennung gestartet...");

        bool hub = detectHubMode();

        if (hub) {
            g_batteryMode = BatteryMode::HUB;
        } else {
            g_batteryMode = BatteryMode::STACK;
        }
    }

    // ---------------------------------------------------------
    // Weiter mit passendem Modus
    // ---------------------------------------------------------
    if (g_batteryMode == BatteryMode::HUB) {
        Log(LOG_INFO, "UART: Starte im HUB-Modus (nur zuhören)");
        commReady = true;      // wir brauchen kein wakeUpConsole()
        busy = false;
        frameReady = false;
        frameValid = false;
        return;                // WICHTIG: Stack-Initialisierung überspringen
    }

    // ---------------------------------------------------------
    // STACK-MODUS → normale Initialisierung
    // ---------------------------------------------------------
    wakeUpConsole();


    commReady     = false;
    busy          = false;
    frameReady    = false;
    frameValid    = false;
    lastCommand   = "";
    lastRawFrame  = "";
    g_invalidCount = 0;

    wakeUpConsole();
}

// ---------------------------------------------------------
void PyUart::switchBaud(int newRate) {
    Log(LOG_DEBUG, "UART: switchBaud(" + String(newRate) + ")");
    Serial2.flush();
    delay(20);
    Serial2.end();
    delay(20);
    Serial2.setRxBufferSize(4096);
    Serial2.begin(newRate, SERIAL_8N1, rxPin, txPin);
    delay(20);
}

// ---------------------------------------------------------
void PyUart::wakeUpConsole() {
    Log(LOG_INFO, "UART: wakeUpConsole()");

    commReady = false;

    switchBaud(1200);
    Serial2.write("~20014682C0048520FCC3\r");
    delay(1000);

    byte nl[] = {0x0E, 0x0A};
    switchBaud(115200);

    for (int i = 0; i < 10; i++) {
        Serial2.write(nl, 2);
        delay(1000);

        if (Serial2.available()) {
            while (Serial2.available()) Serial2.read();
            break;
        }
    }

    commReady = true;
    g_invalidCount = 0;

    Log(LOG_INFO, "UART: wakeUpConsole complete → commReady=true");
}

// ---------------------------------------------------------
int PyUart::readFromSerial() {
    ensureRecvBuff();
    memset(g_szRecvBuff, 0, G_RECV_BUFF_SIZE);
    int recvLen = 0;

    // Wait for the first byte – up to 6 s for slower modules.
    unsigned long startMs = millis();
    while (!Serial2.available() && (millis() - startMs) < 6000UL) {
        delay(10);
    }

    if (!Serial2.available()) {
        Log(LOG_WARN, "UART: timeout waiting for response");
        return 0;
    }

    unsigned long lastByteMs = millis();
    const unsigned long overallTimeoutMs = 35000UL;
    unsigned long idleTimeoutMs = 4000UL;
    if (lastCommand == "pwr") {
        idleTimeoutMs = 5500UL;
    } else if (lastCommand.startsWith("bat")) {
        idleTimeoutMs = 6500UL;
    } else if (lastCommand.startsWith("stat") || lastCommand.startsWith("info")) {
        idleTimeoutMs = 5000UL;
    }

    while ((millis() - startMs) < overallTimeoutMs) {
        bool gotByte = false;

        while (Serial2.available()) {
            char c = (char)Serial2.read();
            gotByte = true;
            lastByteMs = millis();

            if (recvLen + 1 >= (int)G_RECV_BUFF_SIZE) {
                Log(LOG_ERROR, "UART: read overflow (buffer=" + String((uint32_t)G_RECV_BUFF_SIZE) + ", received=" + String(recvLen) + ")");
                return 0;
            }

            g_szRecvBuff[recvLen++] = c;
            g_szRecvBuff[recvLen] = '\0';

            if (strstr(g_szRecvBuff, "Press [Enter] to be continued"))
                Serial2.write("\r");

            if (strstr(g_szRecvBuff, "pylon>"))
                return recvLen;
        }

        if (!gotByte) {
            if (recvLen > 0 && (millis() - lastByteMs) > idleTimeoutMs) {
                // If the frame body is complete, accept even without trailing prompt.
                if (strstr(g_szRecvBuff, "$$")) {
                    Log(LOG_WARN, "UART: frame completed without prompt (len=" + String(recvLen) + ")");
                    return recvLen;
                }
                break;
            }
            delay(10);
        }
    }

    if (strstr(g_szRecvBuff, "pylon>"))
        return recvLen;

    Log(LOG_WARN, "UART: incomplete response (len=" + String(recvLen) + ")");
    Log(LOG_DEBUG, "UART RX len=" + String(recvLen));
    return 0;
}

// ---------------------------------------------------------
bool PyUart::sendCommandAndReadSerialResponse(const char* cmd) {
    // Drain any unsolicited/async console output (event log lines, leftover
    // prompt fragments) that accumulated since the previous command. If these
    // bytes stay in the RX FIFO they get prepended to and overlay the response,
    // which corrupts BAT/PWR frames and causes dropped modules (data gaps).
    // Wait until the RX line has been quiet for a short window so an in-flight
    // log line is fully consumed before we issue the command.
    {
        size_t drained = 0;
        unsigned long startMs = millis();
        unsigned long lastActivity = startMs;
        while ((millis() - startMs) < 1000UL) {
            if (Serial2.available()) {
                while (Serial2.available()) { Serial2.read(); drained++; }
                lastActivity = millis();
            } else if ((millis() - lastActivity) >= 80UL) {
                break;
            } else {
                delay(5);
            }
        }
        if (drained > 0) {
            Log(LOG_DEBUG, "UART: drained " + String((uint32_t)drained) + " stale RX bytes before TX");
        }
    }

    if (cmd && cmd[0]) {
        Log(LOG_DEBUG, "UART TX: '" + String(cmd) + "'");
        Serial2.write(cmd);
    }

    Serial2.write("\n");
    Serial2.flush();

    int len = readFromSerial();
    return (len > 0);
}

// ---------------------------------------------------------
bool PyUart::sendCommand(const char* cmd) {

    if (!commReady) {
        Log(LOG_WARN, "UART: commReady=false → wakeUpConsole()");
        wakeUpConsole();
        if (!commReady) {
            Log(LOG_ERROR, "UART: wakeUpConsole failed");
            return false;
        }
    }

    while (Serial2.available()) Serial2.read();
    delay(100);
    // Extra flush cycle to ensure no stale bytes remain
    while (Serial2.available()) Serial2.read();
    delay(50);

    lastCommand = String(cmd);

    busy       = true;
    frameReady = false;
    frameValid = false;

    if (!sendCommandAndReadSerialResponse(cmd)) {
        busy = false;
        g_invalidCount++;

        Log(LOG_WARN, "UART: no response, invalidCount=" + String(g_invalidCount));

        if (g_invalidCount > 3) {
            commReady = false;
            Log(LOG_ERROR, "UART: too many failures → commReady=false");
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return false;
    }

    lastRawFrame = g_szRecvBuff;
    frameReady   = true;
    frameValid   = isValidFrame(lastRawFrame);

    // Capture the full raw response for the web console *before* the parser
    // can mark frameReady=false. This is independent of frameValid so even
    // commands without a proper $$ frame (help, log, unit, …) are shown in
    // full. Scheduled commands also pass through here, but the console
    // frontend filters by command name + sequence, so polling stays robust.
    if (consoleMutex && xSemaphoreTake(consoleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        consoleFrame = lastRawFrame;
        consoleCmd   = lastCommand;
        consoleSeq++;
        xSemaphoreGive(consoleMutex);
    }

    if (!frameValid) {
        g_invalidCount++;
        Log(LOG_WARN, "UART: invalid frame received");

        if (g_invalidCount > 3) {
            commReady = false;
            Log(LOG_ERROR, "UART: too many invalid frames → commReady=false");
        }

        busy = false;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return false;
    }

    g_invalidCount = 0;

    Log(LOG_INFO, "UART: valid frame received (" + String(lastRawFrame.length()) + " bytes)");

    // ---------------------------------------------------------
    // PARSER DIRECT CALL (NEW ARCHITECTURE)
    // ---------------------------------------------------------
    bool parseOk = true;
    if (frameValid) {

        const String& raw = lastRawFrame;

        // -----------------------------
        // PWR PARSER
        // -----------------------------
        if (lastCommand == "pwr") {
            BatteryStack stack;
            std::vector<BatteryModule> mods;

            ParseResult r = parsePwrFrame(raw, stack, mods);
            parseOk = (r == PARSE_OK);

            if (r == PARSE_OK) {
                if (g_pwrMutex && xSemaphoreTake(g_pwrMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    PwrBuffer* target = pwrUseA ? &pwrB : &pwrA;
                    target->stack = stack;
                    target->modules = std::move(mods);   // move, not deep-copy (avoids transient FieldMap-String duplication)
                    pwrUseA = !pwrUseA;
                    xSemaphoreGive(g_pwrMutex);
                }
                parserHasData = true;
            }
        }

        // -----------------------------
        // BAT PARSER
        // -----------------------------
        else if (lastCommand.startsWith("bat")) {
            BatData out;
            int moduleIndex = lastCommand.substring(3).toInt();

            ParseResult r = parseBatFrame(moduleIndex, raw, out);
            parseOk = (r == PARSE_OK);

            if (r == PARSE_OK) {
                if (g_batMutex && xSemaphoreTake(g_batMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    BatBuffer* target = batUseA ? &batB : &batA;
                    target->cells = lastParsedBatCells;
                    batUseA = !batUseA;
                    xSemaphoreGive(g_batMutex);
                }
                batParserHasData = true;
                batParserModuleIndex = moduleIndex;
            }
        }

        // -----------------------------
        // STAT PARSER
        // -----------------------------
        else if (lastCommand.startsWith("stat")) {
            StatData out;
            int moduleIndex = lastCommand.substring(4).toInt();

            ParseResult r = parseStatFrame(moduleIndex, raw, out);
            parseOk = (r == PARSE_OK);

            if (r == PARSE_OK) {
                // Parser handles buffer swap now
                statParserHasData = true;
                statParserModuleIndex = moduleIndex;

                // per-module buffer – kurzer Timeout, UART nicht blockieren
                if (moduleIndex >= 1 && moduleIndex < MAX_BATTERY_MODULES) {
                    if (g_statMutex && xSemaphoreTake(g_statMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        g_pendingStat[moduleIndex] = out;
                        g_pendingStatReady[moduleIndex] = true;
                        xSemaphoreGive(g_statMutex);
                    }
                }
            }
        }

        // -----------------------------
        // INFO PARSER
        // -----------------------------
        else if (lastCommand.startsWith("info")) {
            InfoData out;
            int moduleIndex = lastCommand.substring(4).toInt();

            ParseResult r = parseInfoFrame(moduleIndex, raw, out);
            parseOk = (r == PARSE_OK);

            if (r == PARSE_OK) {
                // Parser handles buffer swap now
                infoParserHasData = true;
                infoParserModuleIndex = moduleIndex;

                // per-module buffer – kurzer Timeout, UART nicht blockieren
                if (moduleIndex >= 1 && moduleIndex < MAX_BATTERY_MODULES) {
                    if (g_infoMutex && xSemaphoreTake(g_infoMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        g_pendingInfo[moduleIndex] = out;
                        g_pendingInfoReady[moduleIndex] = true;
                        xSemaphoreGive(g_infoMutex);
                    }
                }
            }
        }

        // Frame wurde verarbeitet → nicht erneut parsen
        frameReady = false;
    }

    if (!parseOk) {
        Log(LOG_WARN, "UART: parser rejected frame for command: " + lastCommand);
        busy = false;
        return false;
    }

    busy = false;
    return true;
}

// ---------------------------------------------------------
void PyUart::loop() {}

// ---------------------------------------------------------
String PyUart::getFrame() {
    frameReady = false;
    return lastRawFrame;
}

// ---------------------------------------------------------
void PyUart::getConsoleSnapshot(String& cmd, String& frame, uint32_t& seq) {
    if (consoleMutex && xSemaphoreTake(consoleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        cmd   = consoleCmd;
        frame = consoleFrame;
        seq   = consoleSeq;
        xSemaphoreGive(consoleMutex);
    } else {
        cmd = ""; frame = ""; seq = 0;
    }
}

// ---------------------------------------------------------
void PyUart::recoverConsole() {
    if (g_batteryMode == BatteryMode::HUB) return;

    while (Serial2.available()) Serial2.read();
    delay(20);
    wakeUpConsole();
}
