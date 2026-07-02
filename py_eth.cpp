#include "py_eth.h"
#include "config.h"
#include "py_log.h"

// W5500 SPI Ethernet for ESP32-S3 (Waveshare ESP32-S3-ETH)
// Pins: MISO=12, MOSI=11, SCLK=13, CS=14, RST=9, INT=10
#include <ETH.h>
#include <SPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_mac.h"
#include <esp_heap_caps.h>

// Skip ArduinoOTA/mDNS UDP polling under critically low internal DRAM (see the
// matching guard + rationale in py_wifimanager.cpp).
static constexpr size_t ETH_OTA_MIN_FREE_HEAP = 14000;

using namespace EthManagerModule;

// ----------------------------------------------------
// Internal state
// ----------------------------------------------------
static volatile bool ethGotIP  = false;
static volatile bool ethLinked = false;

// ----------------------------------------------------
// NTP trigger
// ----------------------------------------------------
static void triggerNtpViaEth() {
    if (config.manual_mode) return;

    String ntpSrv;
    if (config.use_gateway_ntp) {
        IPAddress gw = ETH.gatewayIP();
        ntpSrv = (gw != IPAddress(0,0,0,0)) ? gw.toString() : "pool.ntp.org";
    } else if (config.manual_ntp) {
        ntpSrv = config.ntpServer;
    } else {
        ntpSrv = "pool.ntp.org";
    }

    configTime(0, 0, ntpSrv.c_str());
    Log(LOG_INFO, "ETH: NTP triggered, server=" + ntpSrv);
}

// ----------------------------------------------------
// Apply static IP config (called from event handler)
// ----------------------------------------------------
static void applyStaticIp() {
    if (!config.ethUseStaticIP || config.ethIpAddr.length() == 0) return;

    IPAddress ip, mask, gw, dns1;
    if (ip.fromString(config.ethIpAddr)      &&
        mask.fromString(config.ethSubnetMask) &&
        gw.fromString(config.ethGateway)) {

        dns1.fromString(config.ethDns.length() > 0 ? config.ethDns : config.ethGateway);
        ETH.config(ip, gw, mask, dns1);
        Log(LOG_INFO, "ETH: static IP applied: " + config.ethIpAddr);
    } else {
        Log(LOG_WARN, "ETH: static IP parse failed - using DHCP");
    }
}

// ----------------------------------------------------
// ETH event handler
// ----------------------------------------------------
static void onEthEvent(WiFiEvent_t event) {
    switch (event) {

        case ARDUINO_EVENT_ETH_START:
            Log(LOG_INFO, "ETH: adapter started");
            ETH.setHostname(config.hostname.c_str());
            // Apply static IP at adapter start (before DHCP kicks in)
            applyStaticIp();
            break;

        case ARDUINO_EVENT_ETH_CONNECTED:
            ethLinked = true;
            Log(LOG_INFO, "ETH: link up");
            break;

        case ARDUINO_EVENT_ETH_GOT_IP:
            ethGotIP = true;
            Log(LOG_INFO, "ETH: IP = " + ETH.localIP().toString()
                        + "  speed=" + String(ETH.linkSpeed()) + " Mbit/s");

            triggerNtpViaEth();

            // Start OTA once (shared flag avoids double-init with WiFiManager)
            if (!config.otaStarted) {
                config.otaStarted = true;
                ArduinoOTA.setHostname(config.hostname.c_str());
                if (!MDNS.begin(config.hostname.c_str()))
                    Log(LOG_WARN, "ETH: mDNS start failed (may already be running)");
                ArduinoOTA.begin();
                Log(LOG_INFO, "ETH: OTA ready");
            }
            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ethLinked = false;
            ethGotIP  = false;
            Log(LOG_WARN, "ETH: link down");
            break;

        case ARDUINO_EVENT_ETH_STOP:
            ethLinked = false;
            ethGotIP  = false;
            Log(LOG_WARN, "ETH: adapter stopped");
            break;

        default:
            break;
    }
}

// ----------------------------------------------------
// Derive a stable, unique MAC for the W5500 from the
// ESP32 eFuse base MAC.
//
// ESP32 eFuse base MAC (6 bytes):
//   [0..5]  → used for WiFi STA  (byte[5] = base)
//   [0..5]  → WiFi AP  uses base MAC + 1 on byte[5]
//   We use  base MAC + 3 on byte[5] for Ethernet,
//   which is the same convention used by Espressif
//   in their official esp_read_mac() with
//   ESP_MAC_ETH.  This guarantees uniqueness within
//   the same board and is consistent across reboots.
// ----------------------------------------------------
static void buildEthMac(uint8_t mac[6]) {
    esp_read_mac(mac, ESP_MAC_ETH);

    char buf[24];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Log(LOG_INFO, "ETH: MAC = " + String(buf));
}

// ----------------------------------------------------
// Public API
// ----------------------------------------------------

void EthManagerModule::begin() {
    if (!config.useEthernet) {
        Log(LOG_INFO, "ETH: disabled in config - skipping init");
        return;
    }

    Log(LOG_INFO, "ETH: initialising W5500 SPI"
                  " CS="   + String(config.ethCsPin)   +
                  " INT="  + String(config.ethIntPin)  +
                  " RST="  + String(config.ethRstPin)  +
                  " SCLK=" + String(config.ethSclkPin) +
                  " MISO=" + String(config.ethMisoPin) +
                  " MOSI=" + String(config.ethMosiPin));

    WiFi.onEvent(onEthEvent);

    // Build stable MAC derived from ESP32 eFuse and set it on the W5500
    uint8_t ethMac[6];
    buildEthMac(ethMac);
    esp_iface_mac_addr_set(ethMac, ESP_MAC_ETH);

    // Init SPI bus with board-specific pins
    SPI.begin(config.ethSclkPin,
              config.ethMisoPin,
              config.ethMosiPin,
              config.ethCsPin);

    // Start W5500 via Arduino SPI driver
    // Signature (Core 3.x): begin(type, phy_addr, cs, irq, rst, SPIClass&)
    ETH.begin(ETH_PHY_W5500,
              (int32_t)config.ethPhyAddr,
              (int)config.ethCsPin,
              (int)config.ethIntPin,
              (int)config.ethRstPin,
              SPI);

    Log(LOG_INFO, "ETH: begin() done, waiting for link...");
}

void EthManagerModule::loop() {
    if (!config.useEthernet) return;

    if (ethGotIP && config.otaStarted &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= ETH_OTA_MIN_FREE_HEAP) {
        ArduinoOTA.handle();
    }
}

EthStatus EthManagerModule::getStatus() {
    EthStatus s;
    s.linked    = ethLinked;
    s.connected = ethGotIP;
    s.ip        = ethGotIP ? ETH.localIP().toString() : "";
    s.mac       = ETH.macAddress();
    s.speed     = ETH.linkSpeed();
    return s;
}

String EthManagerModule::getStatusJson() {
    EthStatus s = getStatus();

    StaticJsonDocument<256> doc;
    doc["enabled"]   = config.useEthernet;
    doc["linked"]    = s.linked;
    doc["connected"] = s.connected;
    doc["ip"]        = s.ip;
    doc["mac"]       = s.mac;
    doc["speed"]     = s.speed;

    String out;
    serializeJson(doc, out);
    return out;
}
