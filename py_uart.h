#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class PyUart {
public:
    void begin(int rx, int tx);
    void loop();   // intentionally empty

    // Blocking command → fills lastRawFrame
    bool sendCommand(const char* cmd);
    void recoverConsole();

    // Frame access for realtimeTask()
    bool hasFrame() const { return frameReady; }
    bool isFrameValid() const { return frameValid; }
    String getFrame();   // returns lastRawFrame and clears frameReady

    // Status
    bool isReady() const { return commReady; }
    bool isBusy() const { return busy; }
    String getLastCommand() const { return lastCommand; }

    // HUB mode: passive frame listener (non-blocking)
    bool pollHubFrame();

    // Last raw frame for web UI (framedump). Per-type copies were removed:
    // their getters were never called and they churned internal DRAM heap.
    String getLastRawFrame() const { return lastRawFrame; }

    // Console capture: full raw response of the most recently executed command,
    // kept independent of frameReady/parser clearing so the web console never
    // loses lines. seq is bumped on every captured response (valid or not).
    uint32_t getConsoleSeq() const { return consoleSeq; }
    void     getConsoleSnapshot(String& cmd, String& frame, uint32_t& seq);

private:
    void switchBaud(int newRate);
    void wakeUpConsole();
    int  readFromSerial();
    bool sendCommandAndReadSerialResponse(const char* cmd);

    bool commReady = false;
    bool busy = false;
    bool frameReady = false;
    bool frameValid = false;

    // HUB mode passive listening buffer
    String hubBuf;

    int rxPin = -1;
    int txPin = -1;

    String lastCommand;
    String lastRawFrame;

    // Console capture buffer (full raw output, never trimmed by parser)
    String   consoleFrame;
    String   consoleCmd;
    volatile uint32_t consoleSeq = 0;
    SemaphoreHandle_t consoleMutex = nullptr;
};
