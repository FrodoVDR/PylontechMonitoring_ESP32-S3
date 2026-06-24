#pragma once
#include <Arduino.h>

struct WifiStatus {
    String mode;
    bool connected;
    String ssid;
    int rssi;
    String ip;
    String mac;
};


namespace WiFiManagerModule {

    void begin();
    void loop();

    void connect(const String& ssid, const String& pass);
    void resetWiFi();

    void startTemporaryAP(unsigned long durationMs);

    String getStatusJson();
    String scanJson();

    void setManualTime(int year, int month, int day, int hour, int minute, bool dst);
    void applyTimezone(); // Apply current config.timezone to system TZ

    WifiStatus getStatus();

}