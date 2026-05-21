// =========================
// PylontechMonitoring (ESP32-S)
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
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ---- Project Includes ----
#include "esp_log.h"
#include "config.h"
#include "py_wifimanager.h"
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

// =========================
 //  Task 1: Real‑Time Pipeline (Core 0)
 //  UART → Parser → Queue
 // =========================
void realtimeTask(void* parameter) {

    for (;;) {

        // 1) UART must be ready
        if (!py_uart.isReady()) {
            py_uart.begin(16, 17);
            vTaskDelay(100);
            continue;
        }

        // 2) If UART is busy, wait
        if (py_uart.isBusy()) {
            vTaskDelay(1);
            continue;
        }

        // 3) Check if scheduler has a command
        if (!py_scheduler.hasQueuedCommand()) {
            vTaskDelay(1);
            continue;
        }

        // 4) Pop next command
        String cmd = py_scheduler.popNextCommand();
        if (cmd.length() == 0) {
            vTaskDelay(1);
            continue;
        }

        Log(LOG_INFO, "Task1: executing command: " + cmd);

        // 5) Execute UART command (blocking allowed)
        bool ok = py_uart.sendCommand(cmd.c_str());

        if (!ok || !py_uart.hasFrame()) {
            Log(LOG_WARN, "Task1: UART failed for command: " + cmd);
            py_scheduler.lastCommandFinished = millis();
            vTaskDelay(1);
            continue;
        }

        // 6) Get raw frame (Parser läuft in PyUart)
        String raw = py_uart.getFrame();

        // 7) Mark command finished
        py_scheduler.lastCommandFinished =millis();

        // 8) Small delay
        vTaskDelay(1);
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

    for (;;) {
        unsigned long now = millis();

        // 1) Scheduler
        if (now - lastSched >= 20) {
            lastSched = now;
            py_scheduler.loop();
        }

        // 2) MQTT raw queue
        while (xQueueReceive(mqttQueue, &msg, 0) == pdTRUE) {
            py_mqtt.publishRaw(msg.topic, msg.payload);
        }

        // 3) MQTT internal
        if (now - lastMqtt >= 20) {
            lastMqtt = now;
            py_mqtt.loop();
        }

        // 4) Webserver
        if (now - lastWeb >= 20) {
            lastWeb = now;
            WebServerModule_handle();
        }

        // 5) WiFi + System
        if (now - lastSys >= 20) {
            lastSys = now;
            WiFiManagerModule::loop();
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

        // Display-Update (NICHT im Debug-Block!)
        if (parserHasData) {
            auto pwr = pwrUseA ? pwrA : pwrB;

            if (!pwr.modules.empty()) {
                auto &s = pwr.stack;

                display.updatePwr(
                    s.avgVoltage_mV,
                    s.totalCurrent_mA,
                    s.temperature,
                    s.soc
                );
            }
        }

        bool apMode = (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA);

        display.updateWifi(
            WiFi.status() == WL_CONNECTED,
            WiFi.localIP().toString(),
            WiFi.RSSI(),
            apMode
        );

        // Display nur alle 500 ms aktualisieren
        static unsigned long lastDisplay = 0;
        if (now - lastDisplay >= 500) {
            lastDisplay = now;
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

    Log(LOG_INFO, "System booting...");

    // Load configuration (deine bestehende load() kümmert sich um alles)
    config.load();

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

    // Initial command
    py_scheduler.enqueue("pwr");
    Log(LOG_INFO, "Scheduler: initial CMD_PWR enqueued");

    // System Manager
    SystemManager::begin();

    // WiFi + OTA + NTP
    WiFiManagerModule::begin();

    // MQTT
    py_mqtt.begin();

    // SPIFFS
    if (!SPIFFS.begin(false)) {
        Log(LOG_WARN, "SPIFFS mount failed, formatting...");
        SPIFFS.begin(true);
    } else {
        Log(LOG_INFO, "SPIFFS mounted");
    }

    // Webserver
    WebServerModule_begin();

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
        8192,
        NULL,
        2,          // higher priority
        NULL,
        1           // Core 1
    );

    // Start Task 2 (Non‑Critical + OTA + Webserver) auf Core 0
    xTaskCreatePinnedToCore(
        noncriticalTask,
        "NonCritical Task",
        8192,
        NULL,
        1,          // normal priority
        NULL,
        0           // Core 0
    );

    Log(LOG_INFO, "Setup complete");
}

// =========================
//  Arduino Loop (unused)
// =========================
void loop() {
    // Empty – all logic moved to FreeRTOS tasks
    vTaskDelay(1000);
}
