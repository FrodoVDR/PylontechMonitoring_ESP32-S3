#pragma once
#include <Arduino.h>

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
};
