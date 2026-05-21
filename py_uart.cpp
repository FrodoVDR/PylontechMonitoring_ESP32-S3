#include "py_uart.h"
#include "py_log.h"
#include "py_parser_pwr.h"
#include "py_parser_bat.h"
#include "py_parser_stat.h"

#include "config.h"   // enthält PwrBuffer, BatBuffer, StatBuffer + Flags


#define BAT_RX_PIN 16
#define BAT_TX_PIN 17

static char g_szRecvBuff[7000];
static int g_invalidCount = 0;

// ---------------------------------------------------------
static bool isValidFrame(const String& f) {
    if (f.indexOf("@") < 0) return false;
    if (f.indexOf("$$") < 0) return false;
    if (f.indexOf("pylon>") < 0) return false;
    if (f.length() < 40) return false;

    int lines = 0;
    for (int i = 0; i < f.length(); i++)
        if (f[i] == '\n') lines++;
    if (lines < 3) return false;

    if (f.indexOf("Press [Enter]") >= 0 &&
        f.indexOf("Remote command:") < 0)
        return false;

    return true;
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
void PyUart::begin(int rx, int tx) {
    rxPin = rx;
    txPin = tx;

    Serial2.begin(115200, SERIAL_8N1, rxPin, txPin);
    delay(50);

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
    lastPwrFrame  = "";
    lastBatFrame  = "";
    lastStatFrame = "";
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
    memset(g_szRecvBuff, 0, sizeof(g_szRecvBuff));
    int recvLen = 0;

    // Wait for first byte
    for (int i = 0; i < 150 && !Serial2.available(); ++i)
        delay(10);

    if (!Serial2.available()) {
        Log(LOG_WARN, "UART: timeout waiting for response");
        return 0;
    }

    while (Serial2.available()) {
        char buf[256] = "";
        int r = Serial2.readBytesUntil('>', buf, sizeof(buf) - 1);

        if (r > 0) {
            if (r + recvLen + 1 >= (int)sizeof(g_szRecvBuff)) {
                Log(LOG_WARN, "UART: read overflow");
                break;
            }

            strcat(g_szRecvBuff, buf);
            recvLen += r;

            if (strstr(g_szRecvBuff, "$$\r\n\rpylon")) {
                strcat(g_szRecvBuff, ">");
                break;
            }

            if (strstr(g_szRecvBuff, "Press [Enter] to be continued"))
                Serial2.write("\r");

            for (int j = 0; j < 20 && !Serial2.available(); ++j)
                delay(10);
        } else break;
    }

    Log(LOG_DEBUG, "UART RX len=" + String(recvLen));
    return recvLen;
}

// ---------------------------------------------------------
bool PyUart::sendCommandAndReadSerialResponse(const char* cmd) {
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
    delay(10);

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

    lastRawFrame = String(g_szRecvBuff);
    frameReady   = true;
    frameValid   = isValidFrame(lastRawFrame);

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

    if (lastCommand == "pwr")      lastPwrFrame = lastRawFrame;
    else if (lastCommand.startsWith("bat"))  lastBatFrame = lastRawFrame;
    else if (lastCommand.startsWith("stat")) lastStatFrame = lastRawFrame;

    // ---------------------------------------------------------
    // PARSER DIRECT CALL (NEW ARCHITECTURE)
    // ---------------------------------------------------------
    if (frameValid) {

        String raw = lastRawFrame;

        // -----------------------------
        // PWR PARSER
        // -----------------------------
        if (lastCommand == "pwr") {
            BatteryStack stack;
            std::vector<BatteryModule> mods;

            ParseResult r = parsePwrFrame(raw, stack, mods);

            if (r == PARSE_OK) {
                PwrBuffer* target = pwrUseA ? &pwrB : &pwrA;
                target->stack = stack;
                target->modules = mods;
                pwrUseA = !pwrUseA;
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

            if (r == PARSE_OK) {
                BatBuffer* target = batUseA ? &batB : &batA;
                target->cells = lastParsedBatCells;
                batUseA = !batUseA;
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

            if (r == PARSE_OK) {
                StatBuffer* target = statUseA ? &statB : &statA;
                target->stat = out;
                statUseA = !statUseA;
                statParserHasData = true;
                statParserModuleIndex = moduleIndex;
            }
        }

        // Frame wurde verarbeitet → nicht erneut parsen
        frameReady = false;
    }
    

    busy = false;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    return true;
}

// ---------------------------------------------------------
void PyUart::loop() {}

// ---------------------------------------------------------
String PyUart::getFrame() {
    frameReady = false;
    return lastRawFrame;
}
