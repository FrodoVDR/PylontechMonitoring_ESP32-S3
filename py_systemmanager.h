#pragma once
#include <Arduino.h>

namespace SystemManager {

    // Callbacks für Events
    extern std::function<void()> onAPTemporary;
    extern std::function<void()> onWiFiReset;
    extern std::function<void()> onFactoryReset;

    void begin();
    void loop();

    // interne Funktionen
    void handleButton();
    void printCpuStats();


    // Events auslösen
    void triggerAPTemporary();
    void triggerWiFiReset();
    void triggerFactoryReset();

    // BootCounter
    int getBootCounter();
}