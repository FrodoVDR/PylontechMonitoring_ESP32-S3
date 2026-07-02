// =========================
// PylontechMonitoring (ESP32-S3 WROOM)
// Clean architecture with 2 tasks:
//   - Task 1 (Core 0): Real‑time pipeline (UART + Parser)
//   - Task 2 (Core 1): Non‑critical pipeline (Scheduler + MQTT + Webserver + WiFi)
// =========================



// ---- System Includes ----
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <TimeLib.h>
#include <SPIFFS.h>
#include <map>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ---- Project Includes ----
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "config.h"
#include "py_wifimanager.h"
#include "py_eth.h"
#include "wp_webserver.h"
#include "py_systemmanager.h"
#include "py_uart.h"
#include "py_scheduler.h"
#include "py_parser_pwr.h"
#include "py_parser_bat.h"
#include "py_parser_stat.h"
#include "py_log.h"
#include "py_mqtt.h"
#include "py_display.h"
#include "web/api_cache.h"  // für apiCacheInvalidateOnFirmwareChange

// =========================
//  Global Data Structures
// =========================

// Magic Header – MUSS in der .bin stehen
const char OTA_MAGIC_HEADER[] = "PYLONTECH_FW_V1";

//PyDisplay display;

// Global queue handle
QueueHandle_t mqttQueue;
QueueHandle_t rtWakeQueue;

// frameQueue kommt aus config.cpp (extern in config.h deklariert)
extern QueueHandle_t frameQueue;

// Global objectsScheduler
PyUart py_uart;
extern PyScheduler py_scheduler;
extern PyMqtt py_mqtt;

// Runtime telemetry counters (Task1 writes, Task2 reads)
volatile uint32_t g_uartFailCount = 0;
volatile uint32_t g_retryQueuedCount = 0;
volatile uint32_t g_retryLimitCount = 0;

// Persistent crash breadcrumbs (survive PANIC reset)
RTC_NOINIT_ATTR uint32_t g_rtcLastStage;
RTC_NOINIT_ATTR uint32_t g_rtcCrashCount;
RTC_NOINIT_ATTR uint32_t g_rtcCrashMagic;
// Latched stage of the LAST real crash (only written on PANIC/WDT reset, so it
// survives intervening clean/OTA reboots that would overwrite g_rtcLastStage).
RTC_NOINIT_ATTR uint32_t g_rtcCrashStage;

// Separate breadcrumb for the Non-Critical task (core running web/MQTT/system).
// The Real-Time task spins its idle stage (130) every ~1 ms, which used to
// overwrite the SHARED breadcrumb ~20x more often than the 20 ms Non-Critical
// loop -> every crash reported as 130 regardless of the true fault location.
// Keeping a dedicated NRT breadcrumb lets a web/MQTT/system fault surface even
// while the RT task is idle-spinning 130.
RTC_NOINIT_ATTR uint32_t g_rtcLastStageNRT;
RTC_NOINIT_ATTR uint32_t g_rtcCrashStageNRT;

static constexpr uint32_t RTC_CRASH_MAGIC = 0x504D4352; // "PMCR"

// Boot-time snapshot of the reset reason + last crash breadcrumb, captured
// once in setup() so it can be reported via /api/monitoring at any time
// (the web-log ring buffer rotates the boot lines away within minutes).
String g_resetReasonStr = "UNKNOWN";
uint32_t g_bootCrashStage = 0;
const char* g_bootCrashStageName = "unknown";
uint32_t g_bootCrashCount = 0;
uint32_t g_bootCrashStageNRT = 0;
const char* g_bootCrashStageNRTName = "unknown";

static inline void setCrashStage(uint32_t stage) {
    g_rtcLastStage = stage;
}

// Breadcrumb for the Non-Critical task (web/MQTT/system loop). Kept separate
// from the Real-Time task's stage so its idle spin (130) cannot mask a fault
// that actually occurred in the web/MQTT/system pipeline.
static inline void setCrashStageNRT(uint32_t stage) {
    g_rtcLastStageNRT = stage;
}

static const char* crashStageName(uint32_t stage) {
    switch (stage) {
        case 10:  return "setup:start";
        case 20:  return "setup:config_loaded";
        case 30:  return "setup:uart_scheduler_ready";
        case 40:  return "setup:net_ready";
        case 50:  return "setup:tasks_started";
        case 100: return "rt:hub_idle";
        case 110: return "rt:wait_uart_ready";
        case 120: return "rt:wait_uart_busy";
        case 130: return "rt:wait_queue";
        case 140: return "rt:cmd_popped";
        case 150: return "rt:uart_send";
        case 160: return "rt:uart_failed";
        case 170: return "rt:uart_ok";
        case 200: return "nrt:scheduler";
        case 210: return "nrt:mqtt_loop";
        case 220: return "nrt:web_loop";
        case 230: return "nrt:sys_loop";
        case 231: return "nrt:wifi_loop";
        case 232: return "nrt:eth_loop";
        case 233: return "nrt:sysmgr_loop";
        default:  return "unknown";
    }
}

// =========================
 //  Task 1: Real‑Time Pipeline (Core 0)
 //  UART → Parser → Queue
 // =========================
void realtimeTask(void* parameter) {
    // Retry state for fragile module commands (INFO + BAT + PWR).
    uint8_t infoRetryCount[MAX_BATTERY_MODULES + 1] = {0};
    uint8_t batRetryCount[MAX_BATTERY_MODULES + 1] = {0};
    uint8_t pwrRetryCount = 0;

    auto extractModuleIndex = [](const String& cmd) -> int {
        if (cmd.startsWith("stat ") || cmd.startsWith("info ")) {
            return cmd.substring(5).toInt();
        }
        if (cmd.startsWith("bat ")) {
            return cmd.substring(4).toInt();
        }
        return -1;
    };

    for (;;) {

        // -------------------------------------------------------
        // HUB-MODUS: passive listening only – NEVER send commands
        // -------------------------------------------------------
        if (g_batteryMode == BatteryMode::HUB) {
            setCrashStage(100);
            // Flush any commands that were queued before mode was locked
            // (e.g. the initial "pwr" from setup) – do NOT send them.
            if (py_scheduler.hasQueuedCommand()) {
                py_scheduler.clearQueue();
            }

            // Read passively from Serial2; parse when a full frame arrives.
            if (py_uart.pollHubFrame()) {
                BatteryStack stack;
                std::vector<BatteryModule> mods;
                ParseResult r = parsePwrFrame(py_uart.getLastRawFrame(), stack, mods);
                if (r == PARSE_OK) {
                    if (g_pwrMutex && xSemaphoreTake(g_pwrMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        PwrBuffer* target = pwrUseA ? &pwrB : &pwrA;
                        target->stack   = stack;
                        target->modules = std::move(mods);   // move, not deep-copy
                        pwrUseA = !pwrUseA;
                        xSemaphoreGive(g_pwrMutex);
                    }
                    parserHasData = true;
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        // 1) UART must be ready
        if (!py_uart.isReady()) {
            setCrashStage(110);
            py_uart.begin(16, 17);
            vTaskDelay(100);
            continue;
        }

        // 2) If UART is busy, wait
        if (py_uart.isBusy()) {
            setCrashStage(120);
            vTaskDelay(1);
            continue;
        }

        // 3) Check if scheduler has a command
        if (!py_scheduler.hasQueuedCommand()) {
            setCrashStage(130);
            vTaskDelay(1);
            continue;
        }

        // 4) Pop next command
        String cmd = py_scheduler.popNextCommand();
        if (cmd.length() == 0) {
            setCrashStage(140);
            vTaskDelay(1);
            continue;
        }

        Log(LOG_INFO, "Task1: executing command: " + cmd);

        // 5) Execute UART command (blocking, parser runs inside)
        setCrashStage(150);
        bool ok = py_uart.sendCommand(cmd.c_str());

        if (!ok) {
            setCrashStage(160);
            Log(LOG_WARN, "Task1: UART failed for command: " + cmd);
            g_uartFailCount++;
            py_scheduler.reportCommandResult(cmd, false);

            int moduleIndex = extractModuleIndex(cmd);
            uint8_t* retrySlot = nullptr;
            if (moduleIndex >= 1 && moduleIndex <= MAX_BATTERY_MODULES) {
                if (cmd.startsWith("info ")) retrySlot = &infoRetryCount[moduleIndex];
                else if (cmd.startsWith("bat ")) retrySlot = &batRetryCount[moduleIndex];
            }

            // Fast self-healing for transient PWR truncation.
            if (cmd == "pwr") {
                if (pwrRetryCount < 1) {
                    pwrRetryCount++;
                    py_scheduler.enqueue("pwr");
                    g_retryQueuedCount++;
                    Log(LOG_WARN, "Task1: retry queued for command: pwr");
                } else {
                    pwrRetryCount = 0;
                    g_retryLimitCount++;
                    Log(LOG_WARN, "Task1: retry limit reached for command: pwr");
                }
            }

            if (retrySlot != nullptr) {
                // Cap BAT retries low so one flaky module (incomplete frames)
                // cannot monopolise the single realtimeTask and starve STAT/INFO.
                // The normal sweep re-queries the module next cycle anyway.
                uint8_t retryLimit = cmd.startsWith("bat ") ? 2 : 2;
                if (*retrySlot < retryLimit) {
                    (*retrySlot)++;
                    if (cmd.startsWith("bat ")) {
                        py_uart.recoverConsole();
                    }
                    py_scheduler.enqueue(cmd);
                    g_retryQueuedCount++;
                    Log(LOG_WARN, "Task1: retry queued for command: " + cmd);
                } else {
                    *retrySlot = 0;
                    g_retryLimitCount++;
                    Log(LOG_WARN, "Task1: retry limit reached for command: " + cmd);
                }
            }

            py_scheduler.lastCommandFinished = millis();
            // Give the BMS time to recover after a failed command before retrying.
            // STAT/INFO keep the longer settle; BAT/PWR use a shorter one so a
            // flaky module wastes less of the realtimeTask's single-threaded budget.
            int failSettleMs = (cmd.startsWith("stat ") || cmd.startsWith("info ")) ? 4000 : 2000;
            vTaskDelay(failSettleMs / portTICK_PERIOD_MS);
            continue;
        }

        // Command succeeded: clear retry state.
        setCrashStage(170);
        int moduleIndex = extractModuleIndex(cmd);
        if (moduleIndex >= 1 && moduleIndex <= MAX_BATTERY_MODULES) {
            if (cmd.startsWith("info ")) infoRetryCount[moduleIndex] = 0;
            else if (cmd.startsWith("bat ")) batRetryCount[moduleIndex] = 0;
        }
        if (cmd == "pwr") pwrRetryCount = 0;

        py_scheduler.reportCommandResult(cmd, true);

        // 6) Frame was already parsed inside sendCommand()
        // 7) Mark command finished
        py_scheduler.lastCommandFinished = millis();

        // 8) Inter-command delay: INFO/STAT need more settle time than PWR/BAT.
        int settleMs = 1500;
        if (cmd.startsWith("stat ") || cmd.startsWith("info ")) {
            settleMs = 3500;  // Increased from 2500 for better BMS timing
        } else if (cmd.startsWith("bat ")) {
            settleMs = 2200;
        }
        vTaskDelay(settleMs / portTICK_PERIOD_MS);
    }
}

// =========================
//  Task 2: Non‑Critical Pipeline (Core 1)
//  Scheduler + MQTT + Webserver + WiFi
// =========================
void noncriticalTask(void* parameter) {
    MqttMessage msg;

    unsigned long lastSched = 0;
    unsigned long lastMqtt  = 0;
    unsigned long lastWeb   = 0;
    unsigned long lastSys   = 0;
    unsigned long lastRam   = 0;
    unsigned long lastSchedDiag = 0;

    for (;;) {
        unsigned long now = millis();

        // 1) Scheduler
        if (now - lastSched >= 20) {
            setCrashStageNRT(200);
            lastSched = now;
            py_scheduler.loop();
        }

        // 2) MQTT raw queue
        while (xQueueReceive(mqttQueue, &msg, 0) == pdTRUE) {
            py_mqtt.publishRaw(msg.topic, msg.payload);
        }

        // 3) MQTT internal
        if (now - lastMqtt >= 20) {
            setCrashStageNRT(210);
            lastMqtt = now;
            py_mqtt.loop();
        }

        // 4) Webserver
        if (now - lastWeb >= 20) {
            setCrashStageNRT(220);
            lastWeb = now;
            WebServerModule_handle();
        }

        // 5) WiFi + Ethernet + System
        if (now - lastSys >= 20) {
            setCrashStageNRT(230);
            lastSys = now;
            setCrashStageNRT(231);
            WiFiManagerModule::loop();
            setCrashStageNRT(232);
            EthManagerModule::loop();
            setCrashStageNRT(233);
            SystemManager::loop();
        }

        // 6) RAM Debug
        if (config.logDebug) {
            if (now - lastRam >= 5000) {
                lastRam = now;

                multi_heap_info_t info;
                heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

                Log(LOG_DEBUG,
                    String("RAM: free=") + info.total_free_bytes +
                    " largest=" + info.largest_free_block +
                    " min_free=" + info.minimum_free_bytes +
                    " alloc=" + info.total_allocated_bytes
                );
            }
        }

        // 7) Scheduler/UART telemetry (every 30 s)
        if (now - lastSchedDiag >= 30000) {
            lastSchedDiag = now;
            Log(LOG_INFO,
                String("Diag: q=") + String((uint32_t)py_scheduler.queuedCount()) +
                " enq=" + String(py_scheduler.enqueueCount()) +
                " pop=" + String(py_scheduler.popCount()) +
                " dropDup=" + String(py_scheduler.dedupeDropCount()) +
                " dropFull=" + String(py_scheduler.fullDropCount()) +
                " uartFail=" + String(g_uartFailCount) +
                " retryQ=" + String(g_retryQueuedCount) +
                " retryLim=" + String(g_retryLimitCount));
        }

        // Display nur alle 500 ms aktualisieren (WiFi-Abfragen erzeugen IPC-Calls)
        static unsigned long lastDisplay = 0;
        if (now - lastDisplay >= 500) {
            lastDisplay = now;

            // Nur die 4 benötigten Integer lesen – KEIN volles PwrBuffer-Copy!
            // auto pwr = pwrUseA ? pwrA : pwrB  wäre eine unsynchronisierte Kopie
            // von 8 BatteryModules mit std::map (Race condition + Heap-Fragmentation).
            {
                int avgV = 0, totC = 0, temp = 0, soc = 0;
                bool hasPwrDisplay = false;
                if (g_pwrMutex && xSemaphoreTake(g_pwrMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    const PwrBuffer& buf = pwrUseA ? pwrA : pwrB;
                    if (!buf.modules.empty()) {
                        avgV = buf.stack.avgVoltage_mV;
                        totC = buf.stack.totalCurrent_mA;
                        temp = buf.stack.temperature;
                        soc  = buf.stack.soc;
                        hasPwrDisplay = true;
                    }
                    xSemaphoreGive(g_pwrMutex);
                }
                if (hasPwrDisplay) {
                    display.updatePwr(avgV, totC, temp, soc);
                }
            }

            bool apMode = (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA);

            display.updateWifi(
                WiFi.status() == WL_CONNECTED,
                WiFi.localIP().toString(),
                WiFi.RSSI(),
                apMode
            );

            display.loop();
        }


        vTaskDelay(1);
    }
}


// =========================
//  Setup
// =========================
void setup() {
    Serial.begin(115200);
    delay(100);

    // Route medium/large generic heap allocations (web-response Strings, JSON
    // documents, api_cache concatenations) to PSRAM. The compile-time default
    // keeps allocations < 4096 B internal; lowering the threshold to 1024 B
    // frees precious internal DRAM for lwIP/WiFi/DMA and adds crash headroom.
    // Capability-specific requests (MALLOC_CAP_INTERNAL / DMA) are unaffected.
    heap_caps_malloc_extmem_enable(1024);

    // RTC_NOINIT content is undefined after cold boot; initialize once via magic.
    if (g_rtcCrashMagic != RTC_CRASH_MAGIC) {
        g_rtcLastStage = 0;
        g_rtcCrashCount = 0;
        g_rtcCrashStage = 0;
        g_rtcLastStageNRT = 0;
        g_rtcCrashStageNRT = 0;
        g_rtcCrashMagic = RTC_CRASH_MAGIC;
    }

    // Log-Mutex initialisieren (MUSS vor dem ersten Log()-Aufruf stehen)
    WebLogInit();

    // Reset-Grund im Web-Log festhalten (sichtbar in UI ohne serielle Konsole)
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* rrStr = "UNKNOWN";
        switch (rr) {
            case ESP_RST_POWERON:    rrStr = "POWER_ON";      break;
            case ESP_RST_EXT:        rrStr = "EXT_RESET";     break;
            case ESP_RST_SW:         rrStr = "SOFTWARE";      break;
            case ESP_RST_PANIC:      rrStr = "PANIC/EXCEPTION"; break;
            case ESP_RST_INT_WDT:    rrStr = "INT_WATCHDOG";  break;
            case ESP_RST_TASK_WDT:   rrStr = "TASK_WATCHDOG"; break;
            case ESP_RST_WDT:        rrStr = "OTHER_WDT";     break;
            case ESP_RST_DEEPSLEEP:  rrStr = "DEEP_SLEEP";    break;
            case ESP_RST_BROWNOUT:   rrStr = "BROWNOUT";      break;
            default: break;
        }
        Log(LOG_WARN, String("*** RESET REASON: ") + rrStr + " ***");
        if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
            g_rtcCrashCount++;
            // Latch the stage of THIS crash so it survives later clean reboots.
            g_rtcCrashStage = g_rtcLastStage;
            g_rtcCrashStageNRT = g_rtcLastStageNRT;
            Log(LOG_WARN,
                String("*** LAST CRASH STAGE: ") + String(g_rtcCrashStage) +
                " (" + String(crashStageName(g_rtcCrashStage)) + ")" +
                " nrt=" + String(g_rtcCrashStageNRT) +
                " (" + String(crashStageName(g_rtcCrashStageNRT)) + ")" +
                " crashes=" + String(g_rtcCrashCount) + " ***");
        }

        // Snapshot for /api/monitoring (survives log-buffer rotation and clean
        // reboots). g_rtcCrashStage is latched at crash time, so it always
        // reports the LAST real crash regardless of intervening OTA reboots.
        g_resetReasonStr      = rrStr;
        g_bootCrashStage      = g_rtcCrashStage;
        g_bootCrashStageName  = crashStageName(g_rtcCrashStage);
        g_bootCrashCount      = g_rtcCrashCount;
        g_bootCrashStageNRT     = g_rtcCrashStageNRT;
        g_bootCrashStageNRTName = crashStageName(g_rtcCrashStageNRT);
    }

    // Mark the current boot after the previous crash state has been reported.
    setCrashStage(10);

    Log(LOG_INFO, "System booting...");

    // Mutexes für thread-sicheren Zugriff auf lastParsedStat / lastParsedInfo
    g_statMutex = xSemaphoreCreateMutex();
    g_infoMutex = xSemaphoreCreateMutex();
    g_batMutex  = xSemaphoreCreateMutex();
    g_pwrMutex  = xSemaphoreCreateMutex();
    g_healthMutex = xSemaphoreCreateMutex();

    // Load configuration (deine bestehende load() kümmert sich um alles)
    config.load();
    
    // Debug: Syslog-Einstellungen nach dem Laden loggen
    Log(LOG_INFO, 
        String("Syslog loaded: enabled=") + 
        (config.syslogEnabled ? "true" : "false") +
        " server=" + config.syslogServer + 
        " port=" + String(config.syslogPort));

    // Keep syslog state unchanged after panic/watchdog resets so crash diagnostics
    // continue to be exported remotely across reboot cycles.
    setCrashStage(20);

    // Create MQTT queue
    mqttQueue = xQueueCreate(
        50,                      // number of buffered messages
        sizeof(MqttMessage)      // size of each element
    );

    if (mqttQueue == NULL) {
        Log(LOG_ERROR, "MQTT Queue could not be created!");
    }

    // UART + Scheduler
    py_uart.begin(16, 17);      // RX=16, TX=17
    py_scheduler.begin(&py_uart);  
    setCrashStage(30);

    // Initial command
    py_scheduler.enqueue("pwr");
    Log(LOG_INFO, "Scheduler: initial CMD_PWR enqueued");

    // System Manager
    SystemManager::begin();

    // WiFi + OTA + NTP
    WiFiManagerModule::begin();

    // Ethernet (only active when config.useEthernet == true)
    EthManagerModule::begin();

    // MQTT
    py_mqtt.begin();
    setCrashStage(40);

    // SPIFFS
    if (!SPIFFS.begin(false)) {
        Log(LOG_WARN, "SPIFFS mount failed, formatting...");
        SPIFFS.begin(true);
    } else {
        Log(LOG_INFO, "SPIFFS mounted");
    }

    // Webserver
    WebServerModule_begin();

    // Invalidate API caches if firmware version changed since last boot
    apiCacheInvalidateOnFirmwareChange(config.getFirmwareVersionWithBuild());

    // Webserver command callback
    WebServerModule_setCommandCallback([](const String &cmd){
        if (cmd == "pwr") {
            py_scheduler.enqueue("pwr");
            Log(LOG_INFO, "Web command: pwr");
            return String("OK");
        }
        return String("UNKNOWN");
    });
    display.begin();
    display.setBrightness(150);   // z.B. ~80%



    // Start Task 1 (Real‑Time) on Core 1
    xTaskCreatePinnedToCore(
        realtimeTask,
        "RealTime Task",
        24576,
        NULL,
        2,          // higher priority
        NULL,
        1           // Core 1
    );

    // Start Task 2 (Non‑Critical + OTA + Webserver) auf Core 0
    xTaskCreatePinnedToCore(
        noncriticalTask,
        "NonCritical Task",
        24576,
        NULL,
        1,          // normal priority
        NULL,
        0           // Core 0
    );
    setCrashStage(50);

    Log(LOG_INFO, "Setup complete");
}

// =========================
//  Arduino Loop (unused)
// =========================
void loop() {
    // Empty – all logic moved to FreeRTOS tasks
    vTaskDelay(1000);
}
