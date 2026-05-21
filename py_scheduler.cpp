#include "py_scheduler.h"
#include "py_log.h"
#include "py_parser_pwr.h"   // lastParsedStack + lastParsedModules
#include "config.h"

PyScheduler py_scheduler;

void PyScheduler::begin(PyUart* u) {
    uart = u;
    queue.clear();

    bootTime = millis();

    lastPwr  = millis();
    lastBat  = millis();
    lastStat = millis();

    initialPwrDone  = false;
    initialBatDone  = false;
    initialStatDone = false;

    Log(LOG_INFO, "Scheduler: started");
}

void PyScheduler::enqueue(const String& cmd) {
    Log(LOG_DEBUG, "Scheduler: enqueue → " + cmd);
    queue.push_back(cmd);
}

bool PyScheduler::hasQueuedCommand() const {
    return !queue.empty();
}

String PyScheduler::popNextCommand() {
    if (queue.empty()) return "";
    String cmd = queue.front();
    queue.erase(queue.begin());
    Log(LOG_DEBUG, "Scheduler: pop → " + cmd);
    return cmd;
}

void PyScheduler::loop() {
    unsigned long now = millis();

    // ---------------------------------------------------------
    // UNKNOWN-MODUS → Scheduler pausiert + Logging alle 30s
    // ---------------------------------------------------------
    if (g_batteryMode == BatteryMode::UNKNOWN) {
        static unsigned long lastLog = 0;
        if (now - lastLog > 30000) {
            Log(LOG_INFO, "Scheduler: UNKNOWN-Modus → warte auf UART-Erkennung");
            lastLog = now;
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return;
    }

    // ---------------------------------------------------------
    // HUB-MODUS → Scheduler bleibt passiv + Logging alle 30s
    // ---------------------------------------------------------
    if (g_batteryMode == BatteryMode::HUB) {
        static unsigned long lastLog = 0;
        if (now - lastLog > 30000) {
            Log(LOG_INFO, "Scheduler: HUB-Modus aktiv → keine Befehle werden gesendet");
            lastLog = now;
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        return;
    }

    // ---------------------------------------------------------
    // STACK-MODUS → alles wie bisher
    // ---------------------------------------------------------

    // UART busy? Dann warten
    if (uart->isBusy()) return;

    unsigned long sinceBoot = now - bootTime;

    // ---------------------------------------------------------
    // INITIAL SEQUENCE (runs only once)
    // ---------------------------------------------------------

    // 1) PWR at T+15s
    if (!initialPwrDone && sinceBoot >= 15000) {
        enqueue("pwr");
        Log(LOG_INFO, "Scheduler: INITIAL PWR");
        initialPwrDone = true;
        return;
    }

    // 2) BAT at T+25s
    if (initialPwrDone && !initialBatDone && sinceBoot >= 25000) {
        enqueue("bat 1");
        Log(LOG_INFO, "Scheduler: INITIAL BAT");
        initialBatDone = true;
        return;
    }

    // 3) STAT at T+45s
    if (initialBatDone && !initialStatDone && sinceBoot >= 45000) {
        enqueue("stat 1");
        Log(LOG_INFO, "Scheduler: INITIAL STAT");
        initialStatDone = true;
        return;
    }
    // 4) DISCOVERY at T+50s
    if (initialStatDone && !initialDiscoveryDone && sinceBoot >= 50000) {

        Log(LOG_INFO, "Scheduler: INITIAL DISCOVERY triggered");

        discoveryPwrNeeded  = true;
        discoveryBatNeeded  = true;
        discoveryStatNeeded = true;

        initialDiscoveryDone = true;
        return;
    }


    // ---------------------------------------------------------
    // NORMAL CYCLIC SCHEDULING (after initial sequence)
    // ---------------------------------------------------------

    if (!initialStatDone) return;

    // PWR
    if (now - lastPwr >= config.battery.intervalPwr) {
        enqueue("pwr");
        lastPwr = now;
        Log(LOG_INFO, "Scheduler: PWR scheduled");
    }

    // BAT
    if (now - lastBat >= config.battery.intervalBat) {
        if (config.battery.enableBat) {
            for (const auto& m : lastParsedModules) {
                if (!m.present) continue;
                enqueue("bat " + String(m.index));
            }
            Log(LOG_INFO, "Scheduler: BAT scheduled (present modules)");
        }
        lastBat = now;
    }

    // STAT
    if (now - lastStat >= config.battery.intervalStat) {
        if (config.battery.enableStat) {
            for (const auto& m : lastParsedModules) {
                if (!m.present) continue;
                enqueue("stat " + String(m.index));
            }
            Log(LOG_INFO, "Scheduler: STAT scheduled (present modules)");
        }
        lastStat = now;
    }
}
