#pragma once
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

class PyDisplay {
public:
    void begin();
    void loop();

    void updatePwr(int volt_mV, int curr_mA, int temp_mC, int soc);

    void updateWifi(bool connected, const String &ip, int rssi, bool apMode);
    void updateMqtt(bool connected, const String &server);

    void setBrightness(uint8_t value); // 0–255


private:
    void drawStaticUI();
    void drawValues();

    // PWR
    int volt = 0;
    int curr = 0;
    int temp = 0;
    int soc  = 0;

    // WiFi
    bool wifiConnected = false;
    String wifiIP = "";
    int wifiRSSI = 0;

    // MQTT
    bool mqttConnected = false;
    String mqttServer = "";
    String lastHealthColor = "";
    String lastHealthOK = "";
    String lastHealthWarn = "";
    String lastHealthErr = "";
    String lastHealthMsg = "";


    bool needsRedraw = true;
};

extern PyDisplay display;
