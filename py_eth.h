#pragma once
#include <Arduino.h>

// ---------------------------------------------------------
// Status structure for Ethernet
// ---------------------------------------------------------
struct EthStatus {
    bool linked    = false;
    bool connected = false;  // got IP
    String ip;
    String mac;
    int    speed = 0;        // Link speed in Mbit/s
};

// ---------------------------------------------------------
// EthManagerModule
//
// Manages ESP32 RMII Ethernet (LAN8720 / IP101).
// Works alongside WiFiManagerModule: both can be active
// simultaneously. The module is only started when
// config.useEthernet == true.
//
// Pin defaults match WT32-ETH01 (LAN8720):
//   ethPhyAddr = 1, ethPwrPin = 16,
//   ethMdcPin = 23, ethMdioPin = 18
// ---------------------------------------------------------
namespace EthManagerModule {

    void begin();
    void loop();

    EthStatus getStatus();
    String    getStatusJson();

}
