#pragma once
#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "py_uart.h"
#include "config.h"
#include "py_parser_pwr.h"   // für lastParsedStack + lastParsedModules

class PyScheduler {
public:
    void begin(PyUart* u);
    void loop();
    void reportCommandResult(const String& cmd, bool success);

    void triggerExclusiveStat();
    void triggerExclusiveInfo();
    void cancelExclusiveStatPhase(const String& reason = "");

    void enqueue(const String& cmd);
    void clearQueue();
    size_t queuedCount() const;
    uint32_t enqueueCount() const { return statsEnqueue; }
    uint32_t popCount() const { return statsPop; }
    uint32_t dedupeDropCount() const { return statsDropDedupe; }
    uint32_t fullDropCount() const { return statsDropFull; }

    bool   hasQueuedCommand() const;
    String popNextCommand();

    unsigned long lastCommandFinished = 0;

    // Exclusive mode: STAT/INFO running, PWR/BAT paused
    bool isExclusiveMode() const { return exclusiveMode; }

private:
    PyUart* uart = nullptr;
    BatteryMode lockedMode = BatteryMode::UNKNOWN;
    bool modeLockActive = false;

    unsigned long bootTime = 0;

    unsigned long lastPwr  = 0;
    unsigned long lastBat  = 0;
    unsigned long lastStat = 0;
    unsigned long lastInfo = 0;

    bool initialPwrDone  = false;
    bool initialBatDone  = false;
    bool initialStatDone = false;
    bool initialInfoDone = false;
    bool initialDiscoveryDone = false;

    // Exclusive mode for STAT/INFO (pauses PWR/BAT)
    bool exclusiveMode = false;
    bool exclusiveStatPending = false;
    bool exclusiveInfoPending = false;
    int  exclusiveModuleIdx = 0;
    unsigned long exclusiveLastCmd = 0;
    unsigned long exclusivePauseMs = 5000;
    int lastSuccessfulStatModule = 0;

    int pickNextStatModule() const;
    void loadStatRoundRobinState();
    void saveStatRoundRobinState();

    void runExclusiveMode(unsigned long now);

    std::vector<String> queue;
    mutable SemaphoreHandle_t queueMutex = nullptr;
    uint32_t statsEnqueue = 0;
    uint32_t statsPop = 0;
    uint32_t statsDropDedupe = 0;
    uint32_t statsDropFull = 0;

    bool lockQueue(TickType_t timeout = pdMS_TO_TICKS(20)) const;
    void unlockQueue() const;
};

extern PyScheduler py_scheduler;
