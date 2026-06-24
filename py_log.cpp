
#include "py_log.h"
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "esp_log.h"
#include <SPIFFS.h>
#include <WiFiUdp.h>
#include <WiFi.h>
#include "config.h"

extern AppConfig config;

#define SYSLOG_QUEUE_LEN  8
#define SYSLOG_TEXT_MAX   200
#define SYSLOG_HOST_MAX   32
#define SYSLOG_APP_MAX    24

struct SyslogMsg {
    uint8_t  pri;
    char     host[SYSLOG_HOST_MAX];
    char     app[SYSLOG_APP_MAX];
    char     text[SYSLOG_TEXT_MAX];
};

static void syslogTask(void* param);  // forward declaration

// Mutex für thread-sicheren Zugriff auf webLog (wird von Task1 + Task2 gleichzeitig beschrieben)
static SemaphoreHandle_t g_logMutex = nullptr;
static bool g_fileLogEnabled = false;
static const char* kFileLogPath = "/debug.log";
static const char* kLogNs = "log_cfg";
static const char* kFileLogKey = "file_en";
static WiFiUDP g_syslogUdp;
static IPAddress g_syslogIp;
static String g_syslogServerCached;
static uint16_t g_syslogPortCached = 0;
static bool g_syslogTargetValid = false;
static QueueHandle_t g_syslogQueue = nullptr;

//static const int MAX_PLOG = 200;

// ---------------------------------------------------------
// Timestamp: YYYY.MM.DD hh:mm:ss,ms
// ---------------------------------------------------------
String getTimestampWithMs() {
    time_t now;
    time(&now);

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    unsigned long ms = millis() % 1000;

    char buf[40];
    snprintf(
        buf, sizeof(buf),
        "%04d.%02d.%02d %02d:%02d:%02d,%03lu",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec,
        ms
    );

    return String(buf);
}

// ---------------------------------------------------------
// Web-Log Buffer
// ---------------------------------------------------------
static String webLog;

void WebLogInit() {
    g_logMutex = xSemaphoreCreateMutex();
    // Pre-allocate the web-log buffer once so the steady-state append/trim cycle
    // never reallocates. Without this, every WebLog() that grows past the current
    // capacity reallocates on the scarce internal heap, fragmenting it over time.
    webLog.reserve(8400);
    g_syslogQueue = xQueueCreate(SYSLOG_QUEUE_LEN, sizeof(SyslogMsg));
    if (g_syslogQueue == nullptr) {
        Serial.println("[SYSLOG] ERROR: Queue creation failed!");
    }
    BaseType_t taskRet = xTaskCreatePinnedToCore(syslogTask, "SyslogTask", 6144, NULL, 1, NULL, 1);
    if (taskRet != pdPASS) {
        Serial.printf("[SYSLOG] ERROR: Task creation failed: %d\n", taskRet);
    }

    // Restore persisted file-log state so it survives crash/reboot cycles.
    Preferences p;
    p.begin(kLogNs, true);
    g_fileLogEnabled = p.getBool(kFileLogKey, false);
    p.end();
}

static int syslogSeverity(LogLevel lvl) {
    switch (lvl) {
        case LOG_ERROR: return 3;
        case LOG_WARN:  return 4;
        case LOG_INFO:  return 6;
        case LOG_DEBUG: return 7;
    }
    return 6;
}

static bool refreshSyslogTarget() {
    if (config.syslogServer == g_syslogServerCached &&
        config.syslogPort == g_syslogPortCached) {
        return g_syslogTargetValid;
    }

    g_syslogServerCached = config.syslogServer;
    g_syslogPortCached = config.syslogPort;
    g_syslogTargetValid = false;

    if (g_syslogServerCached.isEmpty() || g_syslogPortCached == 0) {
        return false;
    }

    g_syslogTargetValid = g_syslogIp.fromString(g_syslogServerCached);
    return g_syslogTargetValid;
}

// Syslog sender task – runs at low priority on Core 0, never blocks the main tasks
static void syslogTask(void* /*param*/) {
    Serial.printf("[SYSLOG] Task started on Core %d\n", xPortGetCoreID());
    SyslogMsg msg;
    static uint32_t lastDebug = 0;
    
    for (;;) {
        if (xQueueReceive(g_syslogQueue, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
            Serial.printf("[SYSLOG] Queue msg: enabled=%d, wifi=%d\n", config.syslogEnabled, WiFi.status());
            
            if (!config.syslogEnabled) {
                Serial.printf("[SYSLOG] Syslog disabled, skipping\n");
                continue;
            }
            
            if (WiFi.status() != WL_CONNECTED) {
                Serial.printf("[SYSLOG] WiFi not connected: %d\n", WiFi.status());
                continue;
            }
            
            if (!refreshSyslogTarget()) {
                Serial.printf("[SYSLOG] Target invalid: server=%s port=%u\n", 
                         config.syslogServer.c_str(), config.syslogPort);
                continue;
            }

            // RFC3164: <PRI>HOSTNAME APPNAME: MESSAGE
            char pkt[16 + SYSLOG_HOST_MAX + SYSLOG_APP_MAX + SYSLOG_TEXT_MAX + 4];
            snprintf(pkt, sizeof(pkt), "<%u>%s %s: %s",
                     (unsigned)msg.pri, msg.host, msg.app, msg.text);

            Serial.printf("[SYSLOG] Sending to %s:%u -> %s\n", 
                     g_syslogServerCached.c_str(), g_syslogPortCached, pkt);
            
            if (g_syslogUdp.beginPacket(g_syslogIp, g_syslogPortCached)) {
                size_t written = g_syslogUdp.write((const uint8_t*)pkt, strlen(pkt));
                g_syslogUdp.endPacket();
                Serial.printf("[SYSLOG] Sent %u bytes\n", written);
            } else {
                Serial.printf("[SYSLOG] beginPacket() failed\n");
            }
        } else {
            // Debug info every 5 seconds
            uint32_t now = millis();
            if (now - lastDebug > 5000) {
                lastDebug = now;
                Serial.printf("[SYSLOG] Idle: enabled=%d, wifi=%d, target=%d, queue_empty\n",
                         config.syslogEnabled, WiFi.status(), g_syslogTargetValid);
            }
        }
    }
}

static void sendToSyslog(LogLevel lvl, const String& line) {
    if (!config.syslogEnabled) return;
    if (lvl == LOG_DEBUG) return;           // avoid flooding
    if (g_syslogQueue == nullptr) return;

    const int facility = 16; // local0
    SyslogMsg msg;
    msg.pri = (uint8_t)(facility * 8 + syslogSeverity(lvl));

    const char* h = config.hostname.length() ? config.hostname.c_str() : "pylontechmonitor";
    const char* a = config.deviceName.length() ? config.deviceName.c_str() : "Pylontech";
    strncpy(msg.host, h, SYSLOG_HOST_MAX - 1);
    msg.host[SYSLOG_HOST_MAX - 1] = '\0';
    strncpy(msg.app,  a, SYSLOG_APP_MAX - 1);
    msg.app[SYSLOG_APP_MAX - 1] = '\0';
    strncpy(msg.text, line.c_str(), SYSLOG_TEXT_MAX - 1);
    msg.text[SYSLOG_TEXT_MAX - 1] = '\0';

    if (xQueueSendToBack(g_syslogQueue, &msg, 0) == pdTRUE) {
        Serial.printf("[SYSLOG] Queued (lvl=%d): %s\n", lvl, line.c_str());
    } else {
        Serial.printf("[SYSLOG] Queue full, dropping msg\n");
    }
}

void WebLog(const String& msg) {
    if (g_logMutex && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Append in two steps to avoid the temporary (msg + "\n") allocation on
        // every log line. remove() keeps the buffer capacity (memmove only), so
        // together with the reserve() in WebLogInit the buffer stays stable.
        webLog += msg;
        webLog += '\n';
        if (webLog.length() > 8000) {
            webLog.remove(0, 2000);
        }
        xSemaphoreGive(g_logMutex);
    }
}

String WebLogGet() {
    String copy;
    if (g_logMutex && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = webLog;
        xSemaphoreGive(g_logMutex);
    }
    return copy;
}

void WebLogClear() {
    if (g_logMutex && xSemaphoreTake(g_logMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        webLog = "";
        xSemaphoreGive(g_logMutex);
    }
}

bool WebLogFileEnabled() {
    return g_fileLogEnabled;
}

void WebLogFileEnable(bool enable) {
    g_fileLogEnabled = enable;

    // Persist immediately so crash/reboot keeps the selected state.
    // NVS on this device is heavily used; a plain putBool can silently fail
    // to commit, leaving a stale value that is read back on the next boot
    // (observed: file log re-enabling itself after reboot). Write, verify by
    // read-back, and retry once via remove+rewrite if the value did not stick.
    Preferences p;
    p.begin(kLogNs, false);
    p.putBool(kFileLogKey, g_fileLogEnabled);
    bool stored = p.getBool(kFileLogKey, !g_fileLogEnabled);
    if (stored != g_fileLogEnabled) {
        p.remove(kFileLogKey);
        p.putBool(kFileLogKey, g_fileLogEnabled);
        stored = p.getBool(kFileLogKey, !g_fileLogEnabled);
    }
    p.end();

    if (stored != g_fileLogEnabled) {
        Log(LOG_ERROR, "WebLogFileEnable: failed to persist file_en state");
    }
}

String WebLogFilePath() {
    return String(kFileLogPath);
}

bool WebLogFileClear() {
    if (SPIFFS.exists(kFileLogPath)) {
        return SPIFFS.remove(kFileLogPath);
    }
    return true;
}

static void appendToFileLog(const String& line) {
    if (!g_fileLogEnabled) return;

    File f = SPIFFS.open(kFileLogPath, FILE_APPEND);
    if (!f) {
        f = SPIFFS.open(kFileLogPath, FILE_WRITE);
        if (!f) return;
    }

    const size_t maxBytes = 256 * 1024;
    if ((size_t)f.size() > maxBytes) {
        f.close();
        SPIFFS.remove(kFileLogPath);
        f = SPIFFS.open(kFileLogPath, FILE_WRITE);
        if (!f) return;
    }

    f.println(line);
    f.close();
}

// ---------------------------------------------------------
// Main-Logfunction
// ---------------------------------------------------------
void Log(LogLevel lvl, const String& msg) {

    // --- Log-Level-Filter ---
    if ((lvl == LOG_INFO  && !config.logInfo)  ||
        (lvl == LOG_WARN  && !config.logWarn)  ||
        (lvl == LOG_ERROR && !config.logError) ||
        (lvl == LOG_DEBUG && !config.logDebug)) {
        return;
    }

    // --- Zeitstempel ---
    String ts = getTimestampWithMs() + " ";

    // --- Prefix ---
    String prefix;
    switch (lvl) {
        case LOG_INFO:  prefix = "[INFO] ";  break;
        case LOG_WARN:  prefix = "[WARN] ";  break;
        case LOG_ERROR: prefix = "[ERROR] "; break;
        case LOG_DEBUG: prefix = "[DEBUG] "; break;
    }

    String line = ts + prefix + msg;

    // --- ESP_LOG ---
    switch (lvl) {
        case LOG_INFO:  ESP_LOGI("Pylontech", "%s", line.c_str()); break;
        case LOG_WARN:  ESP_LOGW("Pylontech", "%s", line.c_str()); break;
        case LOG_ERROR: ESP_LOGE("Pylontech", "%s", line.c_str()); break;
        case LOG_DEBUG: ESP_LOGD("Pylontech", "%s", line.c_str()); break;
    }

    // --- Web-Log ---
    WebLog(line);

    // --- Optional Syslog (UDP) ---
    Serial.printf("[SYSLOG-DBG] before send: enabled=%d queue=%p\n", 
                  (int)config.syslogEnabled, (void*)g_syslogQueue);
    sendToSyslog(lvl, line);

    // --- Optional SPIFFS file log ---
    appendToFileLog(line);

    // --- Serial ---
    Serial.println(line);
    // Optional persistent log
    //PersistentLog(line);
}

static Preferences plogPrefs;

//void PersistentLog(const String& msg) {
//    if (!persistentLoggingEnabled) return;
//
//    plogPrefs.begin("plog", false);
//
//    int count = plogPrefs.getInt("count", 0);
//    String key = "e" + String(count % MAX_PLOG);

//    plogPrefs.putString(key.c_str(), msg);
//    plogPrefs.putInt("count", count + 1);
//
//    plogPrefs.end();
//}

//String PersistentLogDump() {
//    plogPrefs.begin("plog", true);

//    int count = plogPrefs.getInt("count", 0);
//    int start = max(0, count - MAX_PLOG);

//    String out = "";

//    for (int i = start; i < count; i++) {
//        String key = "e" + String(i % MAX_PLOG);
//        out += plogPrefs.getString(key.c_str(), "") + "\n";
//    }

//    plogPrefs.end();
//    return out;
//}
