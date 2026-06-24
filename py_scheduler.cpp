#include "py_scheduler.h"
#include "py_log.h"
#include "py_parser_pwr.h"   // lastParsedStack + lastParsedModules
#include "config.h"
#include <Preferences.h>

PyScheduler py_scheduler;

static const char* batteryModeToString(BatteryMode mode) {
    switch (mode) {
        case BatteryMode::UNKNOWN: return "UNKNOWN";
        case BatteryMode::STACK:   return "STACK";
        case BatteryMode::HUB:     return "HUB";
        default:                   return "INVALID";
    }
}

static int parseStatModuleFromCommand(const String& cmd) {
    if (!cmd.startsWith("stat ")) return -1;
    int idx = cmd.substring(5).toInt();
    if (idx < 1 || idx > MAX_BATTERY_MODULES) return -1;
    return idx;
}

// Read STAT target module indices from the active PWR buffer under mutex.
// Prefer a stable 1..N range so a transient missing present flag does not
// drop the last module from an exclusive STAT sweep.
static std::vector<int> getPresentModuleIndicesThreadSafe() {
    std::vector<int> out;
    if (!g_pwrMutex) return out;
    if (xSemaphoreTake(g_pwrMutex, pdMS_TO_TICKS(20)) != pdTRUE) return out;

    const PwrBuffer& active = pwrUseA ? pwrA : pwrB;
    int count = active.stack.batteryCount;
    if (config.detectedModules > count) count = config.detectedModules;
    if (count > config.battery.maxModules) count = config.battery.maxModules;

    if (count > 0) {
        for (int i = 1; i <= count; ++i) {
            out.push_back(i);
        }
    } else {
        for (const auto& m : active.modules) {
            if (!m.present || m.index <= 0) continue;

            bool exists = false;
            for (int idx : out) {
                if (idx == m.index) {
                    exists = true;
                    break;
                }
            }
            if (!exists) out.push_back(m.index);
        }
    }

    xSemaphoreGive(g_pwrMutex);
    return out;
}

// Determine BAT query targets from the active PWR snapshot under mutex.
// Prefer a stable range 1..batteryCount to avoid missing modules when
// transient 'present' flags are incomplete for a single cycle.
static std::vector<int> getBatTargetModuleIndicesThreadSafe(uint8_t maxModules) {
    std::vector<int> out;
    if (!g_pwrMutex) return out;
    if (xSemaphoreTake(g_pwrMutex, pdMS_TO_TICKS(20)) != pdTRUE) return out;

    const PwrBuffer& active = pwrUseA ? pwrA : pwrB;
    int count = active.stack.batteryCount;
    if (config.detectedModules > count) {
        count = config.detectedModules;
    }

    if (count > 0) {
        if (count > maxModules) count = maxModules;
        for (int i = 1; i <= count; ++i) {
            out.push_back(i);
        }
    } else {
        // Fallback: use currently present module indices if count is unknown.
        for (const auto& m : active.modules) {
            if (!m.present || m.index <= 0) continue;

            bool exists = false;
            for (int idx : out) {
                if (idx == m.index) {
                    exists = true;
                    break;
                }
            }
            if (!exists) out.push_back(m.index);
        }
    }

    xSemaphoreGive(g_pwrMutex);
    return out;
}

bool PyScheduler::lockQueue(TickType_t timeout) const {
    if (!queueMutex) return false;
    return xSemaphoreTake(queueMutex, timeout) == pdTRUE;
}

void PyScheduler::unlockQueue() const {
    if (queueMutex) xSemaphoreGive(queueMutex);
}

void PyScheduler::clearQueue() {
    if (!lockQueue()) return;
    queue.clear();
    unlockQueue();
}

size_t PyScheduler::queuedCount() const {
    if (!lockQueue()) return 0;
    size_t n = queue.size();
    unlockQueue();
    return n;
}

void PyScheduler::begin(PyUart* u) {
    uart = u;
    if (!queueMutex) {
        queueMutex = xSemaphoreCreateMutex();
    }
    clearQueue();
    loadStatRoundRobinState();

    bootTime = millis();

    lastPwr  = millis();
    lastBat  = millis();
    lastStat = millis();
    lastInfo = millis();

    initialPwrDone  = false;
    initialBatDone  = false;
    initialStatDone = false;
    initialInfoDone = false;

    // Freeze mode after startup to prevent sporadic runtime flips from
    // stalling scheduling (e.g. unintended STACK -> HUB switch).
    if (g_batteryMode != BatteryMode::UNKNOWN) {
        lockedMode = g_batteryMode;
        modeLockActive = true;
        Log(LOG_INFO, "Scheduler: mode lock active");
    } else {
        lockedMode = BatteryMode::UNKNOWN;
        modeLockActive = false;
    }

    Log(LOG_INFO, "Scheduler: started");
}

void PyScheduler::loadStatRoundRobinState() {
    Preferences p;
    p.begin("sched_stat", true);
    int stored = (int)p.getUChar("last_ok_mod", 0);
    p.end();

    if (stored < 0 || stored > MAX_BATTERY_MODULES) {
        stored = 0;
    }
    lastSuccessfulStatModule = stored;

    Log(LOG_INFO,
        "Scheduler: STAT RR state loaded, last_ok_mod=" +
        String(lastSuccessfulStatModule));
}

void PyScheduler::saveStatRoundRobinState() {
    Preferences p;
    p.begin("sched_stat", false);
    p.putUChar("last_ok_mod", (uint8_t)lastSuccessfulStatModule);
    p.end();
}

int PyScheduler::pickNextStatModule() const {
    std::vector<int> presentModules = getPresentModuleIndicesThreadSafe();
    if (presentModules.empty()) return 1;

    if (lastSuccessfulStatModule <= 0) {
        return presentModules.front();
    }

    for (size_t i = 0; i < presentModules.size(); ++i) {
        if (presentModules[i] == lastSuccessfulStatModule) {
            size_t next = (i + 1) % presentModules.size();
            return presentModules[next];
        }
    }

    return presentModules.front();
}

void PyScheduler::reportCommandResult(const String& cmd, bool success) {
    int statModule = parseStatModuleFromCommand(cmd);
    if (!success || statModule < 1) return;

    if (lastSuccessfulStatModule != statModule) {
        lastSuccessfulStatModule = statModule;
        saveStatRoundRobinState();
    }
}

void PyScheduler::enqueue(const String& cmd) {
    Log(LOG_DEBUG, "Scheduler: enqueue → " + cmd);
    if (!lockQueue()) {
        Log(LOG_WARN, "Scheduler: enqueue lock timeout");
        return;
    }

    // Avoid queue storms from repeated refresh triggers.
    for (const auto& q : queue) {
        if (q == cmd) {
            statsDropDedupe++;
            unlockQueue();
            return;
        }
    }

    if (queue.size() >= 64) {
        statsDropFull++;
        Log(LOG_WARN, "Scheduler: queue full, dropping command: " + cmd);
        unlockQueue();
        return;
    }

    queue.push_back(cmd);
    statsEnqueue++;
    unlockQueue();
}

void PyScheduler::triggerExclusiveStat() {
    if (g_batteryMode != BatteryMode::STACK) {
        Log(LOG_WARN, "Scheduler: STAT refresh ignored (not in STACK mode)");
        return;
    }

    int lastOk = lastSuccessfulStatModule;
    int module = pickNextStatModule();
    enqueue("stat " + String(module));
    lastStat = millis();
    Log(LOG_INFO,
        "Scheduler: manual STAT RR last_ok_mod=" + String(lastOk) +
        " -> next=" + String(module));
}

void PyScheduler::triggerExclusiveInfo() {
    if (g_batteryMode != BatteryMode::STACK) {
        Log(LOG_WARN, "Scheduler: INFO refresh ignored (not in STACK mode)");
        return;
    }

    clearQueue();
    exclusiveMode = true;
    exclusiveStatPending = false;
    exclusiveInfoPending = true;
    exclusiveModuleIdx = 0;
    exclusiveLastCmd = 0;
    exclusivePauseMs = 1200;

    // Pause normal cyclic traffic while manual INFO run executes.
    lastPwr = millis();
    lastBat = millis();
    lastInfo = millis();

    Log(LOG_INFO, "Scheduler: manual EXCLUSIVE INFO triggered");
}

void PyScheduler::cancelExclusiveStatPhase(const String& reason) {
    size_t removed = 0;
    if (lockQueue()) {
        auto it = queue.begin();
        while (it != queue.end()) {
            if (it->startsWith("stat ")) {
                it = queue.erase(it);
                removed++;
            } else {
                ++it;
            }
        }
        unlockQueue();
    }

    if (exclusiveStatPending) {
        exclusiveStatPending = false;
        exclusiveModuleIdx = 0;

        String msg = "Scheduler: EXCLUSIVE STAT aborted";
        if (reason.length() > 0) msg += " (" + reason + ")";
        if (removed > 0) msg += ", removed=" + String((int)removed);
        Log(LOG_WARN, msg);
    }

    if (exclusiveMode && !exclusiveStatPending && !exclusiveInfoPending) {
        exclusiveMode = false;
        exclusivePauseMs = 5000;
        lastPwr = millis();
        lastBat = millis();
        Log(LOG_INFO, "Scheduler: EXCLUSIVE mode ended → resuming normal");
    }
}

bool PyScheduler::hasQueuedCommand() const {
    if (!lockQueue()) return false;
    bool has = !queue.empty();
    unlockQueue();
    return has;
}

String PyScheduler::popNextCommand() {
    if (!lockQueue()) return "";
    if (queue.empty()) {
        unlockQueue();
        return "";
    }
    String cmd = queue.front();
    queue.erase(queue.begin());
    statsPop++;
    unlockQueue();
    Log(LOG_DEBUG, "Scheduler: pop → " + cmd);
    return cmd;
}

void PyScheduler::loop() {
    unsigned long now = millis();

    // Latch first known mode, then keep it stable for runtime safety.
    if (!modeLockActive && g_batteryMode != BatteryMode::UNKNOWN) {
        lockedMode = g_batteryMode;
        modeLockActive = true;
        Log(LOG_INFO, "Scheduler: mode lock latched");
    }
    if (modeLockActive && g_batteryMode != lockedMode) {
        Log(LOG_WARN,
            "Scheduler: unexpected battery mode change detected (current=" +
            String(batteryModeToString(g_batteryMode)) +
            ", locked=" + String(batteryModeToString(lockedMode)) +
            ") -> restoring locked mode");
        g_batteryMode = lockedMode;
    }

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
        // Keep initial sequence lightweight and continue round-robin after reboot.
        int lastOk = lastSuccessfulStatModule;
        int statModule = pickNextStatModule();
        enqueue("stat " + String(statModule));
        Log(LOG_INFO,
            "Scheduler: INITIAL STAT RR last_ok_mod=" + String(lastOk) +
            " -> next=" + String(statModule));
        initialStatDone = true;
        return;
    }

    // 4) INFO at T+55s
    if (initialStatDone && !initialInfoDone && sinceBoot >= 55000) {
        // Keep initial sequence lightweight: one INFO command only.
        enqueue("info 1");
        Log(LOG_INFO, "Scheduler: INITIAL INFO");
        initialInfoDone = true;
        return;
    }

    // 5) DISCOVERY at T+65s
    if (initialInfoDone && !initialDiscoveryDone && sinceBoot >= 65000) {

        Log(LOG_INFO, "Scheduler: INITIAL DISCOVERY triggered");

        discoveryPwrNeeded  = true;
        discoveryBatNeeded  = true;
        discoveryStatNeeded = true;
        discoveryInfoNeeded = true;

        initialDiscoveryDone = true;
        return;
    }


    // ---------------------------------------------------------
    // NORMAL CYCLIC SCHEDULING (after initial sequence)
    // ---------------------------------------------------------

    // Do not start normal cyclic scheduling until initial sequence
    // (PWR/BAT/STAT/INFO + discovery trigger) has fully completed.
    if (!initialDiscoveryDone) return;

    // ---------------------------------------------------------
    // EXCLUSIVE MODE: STAT/INFO running → PWR/BAT paused
    // ---------------------------------------------------------
    if (exclusiveMode) {
        runExclusiveMode(now);
        return;
    }

    // PWR
    if (now - lastPwr >= config.battery.intervalPwr) {
        enqueue("pwr");
        lastPwr = now;
        Log(LOG_INFO, "Scheduler: PWR scheduled");
    }

    // BAT
    if (now - lastBat >= config.battery.intervalBat) {
        if (config.battery.enableBat) {
            std::vector<int> batTargets = getBatTargetModuleIndicesThreadSafe(config.battery.maxModules);
            for (int idx : batTargets) {
                enqueue("bat " + String(idx));
            }
            Log(LOG_INFO, "Scheduler: BAT scheduled (" + String((int)batTargets.size()) + " modules)");
        }
        lastBat = now;
    }

    // STAT – one module per interval (round-robin, persisted on success)
    if (now - lastStat >= config.battery.intervalStat) {
        if (config.battery.enableStat) {
            int lastOk = lastSuccessfulStatModule;
            int statModule = pickNextStatModule();
            enqueue("stat " + String(statModule));
            Log(LOG_INFO,
                "Scheduler: STAT scheduled RR last_ok_mod=" + String(lastOk) +
                " -> next=" + String(statModule));

            // INFO can still run as its own exclusive phase when both are due.
            if (config.battery.enableInfo && (now - lastInfo >= config.battery.intervalInfo)) {
                exclusiveMode = true;
                exclusiveStatPending = false;
                exclusiveInfoPending = true;
                exclusiveModuleIdx = 0;
                exclusiveLastCmd = 0;
                lastInfo = now;
                Log(LOG_INFO, "Scheduler: entering EXCLUSIVE mode for INFO (queue preserved)");
            }
        }
        lastStat = now;
    }
    // INFO alone (if STAT not due)
    else if (now - lastInfo >= config.battery.intervalInfo) {
        if (config.battery.enableInfo) {
            exclusiveMode = true;
            exclusiveStatPending = false;
            exclusiveInfoPending = true;
            exclusiveModuleIdx = 0;
            exclusiveLastCmd = 0;
            // Keep queued BAT/PWR commands to avoid starving BAT coverage.
            Log(LOG_INFO, "Scheduler: entering EXCLUSIVE mode for INFO (queue preserved)");
        }
        lastInfo = now;
    }
}

// ---------------------------------------------------------
// Exclusive mode: process STAT/INFO one module at a time
// with pauses between each command
// ---------------------------------------------------------
void PyScheduler::runExclusiveMode(unsigned long now) {
    // Wait for UART + pause between commands
    if (uart->isBusy()) return;
    if (exclusiveLastCmd != 0 && (now - exclusiveLastCmd) < exclusivePauseMs) return;

    // Count present modules
    std::vector<int> presentModules = getPresentModuleIndicesThreadSafe();
    if (presentModules.empty()) presentModules.push_back(1);  // Fallback

    // INFO phase
    if (exclusiveInfoPending) {
        if (exclusiveModuleIdx < (int)presentModules.size()) {
            enqueue("info " + String(presentModules[exclusiveModuleIdx]));
            Log(LOG_INFO, "Scheduler: EXCLUSIVE info " + String(presentModules[exclusiveModuleIdx]));
            exclusiveModuleIdx++;
            exclusiveLastCmd = now;
            return;
        }
        // INFO done
        exclusiveInfoPending = false;
        exclusiveModuleIdx = 0;
        Log(LOG_INFO, "Scheduler: EXCLUSIVE INFO complete");
    }

    // STAT phase
    if (exclusiveStatPending) {
        if (exclusiveModuleIdx < (int)presentModules.size()) {
            enqueue("stat " + String(presentModules[exclusiveModuleIdx]));
            Log(LOG_INFO, "Scheduler: EXCLUSIVE stat " + String(presentModules[exclusiveModuleIdx]));
            exclusiveModuleIdx++;
            exclusiveLastCmd = now;
            return;
        }
        // STAT done
        exclusiveStatPending = false;
        Log(LOG_INFO, "Scheduler: EXCLUSIVE STAT complete");
    }

    // All done → exit exclusive mode, resume normal scheduling
    exclusiveMode = false;
    exclusivePauseMs = 12000;  // Keep timing consistent
    lastPwr = now;  // Reset timers so PWR/BAT don't flood immediately
    lastBat = now;
    Log(LOG_INFO, "Scheduler: EXCLUSIVE mode ended → resuming normal");
}
