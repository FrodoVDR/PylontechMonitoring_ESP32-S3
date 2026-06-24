#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "nvs_flash.h"
#include "py_systemmanager.h"
#include "py_log.h"          // Log()
#include "py_wifimanager.h"  // WiFi reset
#include "config.h"          // zentrale Konfig

// ----------------------------------------------------
// FreeRTOS CPU Runtime Stats
// ----------------------------------------------------
extern "C" uint32_t ulHighFrequencyTimerTicks = 0;

void IRAM_ATTR highFreqTimer(void* arg) {
    ulHighFrequencyTimerTicks++;
}

// ----------------------------------------------------
// Konfiguration
// ----------------------------------------------------
static const uint8_t BUTTON_PIN = 0;   // BOOT-Button
static const unsigned long LONG_PRESS_TIME = 15000; // 15s
static const unsigned long SHORT_PRESS_MAX = 1000;  // <1s
static const unsigned long MULTI_PRESS_TIMEOUT = 1000; // 1s Pause

// ----------------------------------------------------
// Interne Variablen
// ----------------------------------------------------
static unsigned long buttonPressStart = 0;
static unsigned long lastButtonRelease = 0;
static int shortPressCount = 0;
static bool buttonWasPressed = false;

// Boot-Counter
static Preferences prefs;
static int bootCounter = 0;

// ----------------------------------------------------
// Initialisierung
// ----------------------------------------------------
void SystemManager::begin() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    prefs.begin("system", false);
    bootCounter = prefs.getInt("bootcount", 0);
    bootCounter++;
    prefs.putInt("bootcount", bootCounter);
    prefs.end();

    Log(LOG_INFO, "SystemManager: BootCounter = " + String(bootCounter));
    // ----------------------------------------------------
    // High-Frequency Timer für FreeRTOS CPU-Statistiken
    // ----------------------------------------------------
    const esp_timer_create_args_t timerArgs = {
        .callback = &highFreqTimer,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cpu_timer"
    };

    esp_timer_handle_t timerHandle;
    esp_timer_create(&timerArgs, &timerHandle);

    // 1 µs Takt (1 MHz)
    esp_timer_start_periodic(timerHandle, 100);

    Log(LOG_INFO, "SystemManager: CPU Runtime Stats aktiviert");

}

// ----------------------------------------------------
// Button-Handling
// ----------------------------------------------------
void SystemManager::loop() {
    handleButton();

    static unsigned long lastCpuPrint = 0;
    if (config.logDebug && (millis() - lastCpuPrint > 30000)) {
        lastCpuPrint = millis();
        printCpuStats();
    }
}


// ----------------------------------------------------
// Button-Auswertung
// ----------------------------------------------------
void SystemManager::handleButton() {
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    unsigned long now = millis();

    // --- Button gedrückt ---
    if (pressed && !buttonWasPressed) {
        buttonWasPressed = true;
        buttonPressStart = now;
    }

    // --- Button losgelassen ---
    if (!pressed && buttonWasPressed) {
        buttonWasPressed = false;
        unsigned long pressDuration = now - buttonPressStart;

        // Langer Druck → Factory Reset
        if (pressDuration > LONG_PRESS_TIME) {
            Log(LOG_WARN, "SystemManager: LONG PRESS → FACTORY RESET");
            triggerFactoryReset();
            return;
        }

        // Kurzer Druck → zählen
        if (pressDuration < SHORT_PRESS_MAX) {
            shortPressCount++;
            lastButtonRelease = now;
        }
    }

    // --- Auswertung der kurzen Drücke ---
    if (shortPressCount > 0 && (now - lastButtonRelease > MULTI_PRESS_TIMEOUT)) {

        if (shortPressCount == 1) {
            Log(LOG_INFO, "SystemManager: 1x short press → AP TEMPORARY");
            triggerAPTemporary();
        }

        if (shortPressCount >= 5) {
            Log(LOG_WARN, "SystemManager: 5x short press → WIFI RESET");
            triggerWiFiReset();
        }

        shortPressCount = 0;
    }
}

// ----------------------------------------------------
// Event-Funktionen
// ----------------------------------------------------
void SystemManager::triggerAPTemporary() {
    if (onAPTemporary) onAPTemporary();
}

void SystemManager::triggerWiFiReset() {
    if (onWiFiReset) onWiFiReset();
}

void SystemManager::triggerFactoryReset() {

    Log(LOG_ERROR, "SystemManager: FACTORY RESET initiated");
    delay(200);

    // ----------------------------------------------------
    // ALLE gespeicherten Daten löschen
    // ----------------------------------------------------
    Preferences p;

    Log(LOG_WARN, "SystemManager: FULL NVS ERASE");
    nvs_flash_erase();
    nvs_flash_init();
    //ESP.restart();



    // ----------------------------------------------------
    // Optional: WiFi trennen
    // ----------------------------------------------------
    //WiFi.disconnect(true);
    //WiFi.mode(WIFI_AP_STA);

    Log(LOG_WARN, "SystemManager: all data cleared → restarting...");
    delay(500);

    ESP.restart();
}

// ----------------------------------------------------
// BootCounter Getter
// ----------------------------------------------------
int SystemManager::getBootCounter() {
    return bootCounter;
}

void SystemManager::printCpuStats() {
    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));

    vTaskGetRunTimeStats(buffer);

    // If the output was truncated, skip parsing to avoid undefined strtok/sscanf behavior.
    if (buffer[sizeof(buffer) - 1] != '\0') {
        Log(LOG_WARN, "CPU stats truncated; skipping parse");
        return;
    }

    // Werte extrahieren
    int idle0 = -1;
    int idle1 = -1;

    char* line = strtok(buffer, "\n");
    while (line != nullptr) {
        if (strstr(line, "IDLE0") != nullptr) {
            if (sscanf(line, "IDLE0 %*u %d%%", &idle0) != 1) idle0 = -1;
        }
        if (strstr(line, "IDLE1") != nullptr) {
            if (sscanf(line, "IDLE1 %*u %d%%", &idle1) != 1) idle1 = -1;
        }
        line = strtok(nullptr, "\n");
    }

    if (idle0 >= 0 && idle1 >= 0) {
        int load0 = 100 - idle0;
        int load1 = 100 - idle1;

        Log(LOG_DEBUG,
            "CPU: idle0=" + String(idle0) + "% idle1=" + String(idle1) +
            "% load0=" + String(load0) + "% load1=" + String(load1) + "%");
    }
}



// ----------------------------------------------------
// Callback-Variablen
// ----------------------------------------------------
std::function<void()> SystemManager::onAPTemporary = nullptr;
std::function<void()> SystemManager::onWiFiReset = nullptr;
std::function<void()> SystemManager::onFactoryReset = nullptr;