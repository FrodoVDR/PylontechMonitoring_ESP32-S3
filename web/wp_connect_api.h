#pragma once
#include "api_core.h"
#include <ArduinoJson.h>
#include "../py_wifimanager.h"
#include "../py_eth.h"
#include "../py_mqtt.h"
#include "../config.h"

extern PyMqtt py_mqtt;

// ---------------------------------------------------------
// WiFi API
// ---------------------------------------------------------
static void apiWifiGet() {
    WifiStatus s = WiFiManagerModule::getStatus();

    StaticJsonDocument<256> doc;
    doc["connected"] = s.connected;
    doc["ssid"]      = s.ssid;
    doc["rssi"]      = s.rssi;
    doc["ip"]        = s.ip;
    doc["mac"]       = s.mac;

    String out;
    serializeJson(doc, out);
    apiJson(out);
}

static void apiWifiPost() {
    if (!server.hasArg("plain"))
        return apiError(400, F("Missing body"));

    StaticJsonDocument<256> req;
    if (deserializeJson(req, server.arg("plain")))
        return apiError(400, F("Invalid JSON"));

    String ssid = req["ssid"] | "";
    String pass = req["pass"] | "";

    if (ssid.isEmpty())
        return apiError(400, F("SSID missing"));

    WiFiManagerModule::connect(ssid, pass);
    apiText(F("Connecting…"));
}

static void apiWifiScan() {
    apiJson(WiFiManagerModule::scanJson());
}

// ---------------------------------------------------------
// MQTT API
// ---------------------------------------------------------
static void apiMqttGet() {
    StaticJsonDocument<256> doc;

    doc["enabled"] = config.mqtt.enabled;
    doc["server"]  = config.mqtt.server;
    doc["port"]    = config.mqtt.port;
    doc["user"]    = config.mqtt.user;
    doc["topic"]   = config.mqtt.prefix;

    String out;
    serializeJson(doc, out);
    apiJson(out);
}

static void apiMqttPost() {
    if (!server.hasArg("plain"))
        return apiError(400, F("Missing body"));

    StaticJsonDocument<256> req;
    if (deserializeJson(req, server.arg("plain")))
        return apiError(400, F("Invalid JSON"));

    config.mqtt.enabled = req["enabled"] | false;
    config.mqtt.server  = req["server"]  | "";
    config.mqtt.port    = req["port"]    | 1883;
    config.mqtt.user    = req["user"]    | "";

    String pass = req["pass"] | "";
    if (!pass.isEmpty()) config.mqtt.pass = pass;

    config.mqtt.prefix = req["topic"] | "Pylontech";

    config.save();
    py_mqtt.begin();

    apiText(F("MQTT saved"));
}

// ---------------------------------------------------------
// TIME / NTP API
// ---------------------------------------------------------
static void apiTimeGet() {
    StaticJsonDocument<256> doc;

    doc["manual_mode"]     = config.manual_mode;
    doc["manual_date"]     = config.manual_date;
    doc["manual_time"]     = config.manual_time;
    doc["manual_dst"]      = config.manual_dst;

    doc["use_gateway_ntp"] = config.use_gateway_ntp;
    doc["manual_ntp"]      = config.manual_ntp;
    doc["server"]          = config.ntpServer;

    doc["timezone"]        = config.timezone;

    String out;
    serializeJson(doc, out);
    apiJson(out);
}

static void apiTimePost() {
    if (!server.hasArg("plain"))
        return apiError(400, F("Missing body"));

    StaticJsonDocument<256> req;
    if (deserializeJson(req, server.arg("plain")))
        return apiError(400, F("Invalid JSON"));

    config.manual_mode     = req["manual_mode"]     | false;
    config.manual_date     = req["manual_date"]     | "";
    config.manual_time     = req["manual_time"]     | "";
    config.manual_dst      = req["manual_dst"]      | false;

    config.use_gateway_ntp = req["use_gateway_ntp"] | true;
    config.manual_ntp      = req["manual_ntp"]      | false;
    config.ntpServer       = req["server"]          | "pool.ntp.org";

    config.timezone        = req["timezone"]        | "Europe/Berlin";

    config.save();
    WiFiManagerModule::applyTimezone(); // Apply new timezone immediately
    apiText(F("Time saved"));
}

// ---------------------------------------------------------
// NETWORK API  (WiFi static IP only)
// ---------------------------------------------------------
static void apiNetworkGet() {
    StaticJsonDocument<256> doc;

    doc["dhcp"] = !config.useStaticIP;
    doc["ip"]   = config.ipAddr;
    doc["mask"] = config.subnetMask;
    doc["gw"]   = config.gateway;
    doc["dns"]  = config.dns;

    String out;
    serializeJson(doc, out);
    apiJson(out);
}

static void apiNetworkPost() {
    if (!server.hasArg("plain"))
        return apiError(400, F("Missing body"));

    StaticJsonDocument<256> req;
    if (deserializeJson(req, server.arg("plain")))
        return apiError(400, F("Invalid JSON"));

    config.useStaticIP = !(req["dhcp"] | true);
    config.ipAddr      = req["ip"]   | "";
    config.subnetMask  = req["mask"] | "";
    config.gateway     = req["gw"]   | "";
    config.dns         = req["dns"]  | "";

    config.save();
    apiText(F("WiFi network config saved"));
}

// ---------------------------------------------------------
// ETHERNET API
// ---------------------------------------------------------
static void apiEthGet() {
    String statusJson = EthManagerModule::getStatusJson();

    StaticJsonDocument<512> doc;
    deserializeJson(doc, statusJson);

    doc["phy_addr"]  = config.ethPhyAddr;
    doc["miso_pin"]  = config.ethMisoPin;
    doc["mosi_pin"]  = config.ethMosiPin;
    doc["sclk_pin"]  = config.ethSclkPin;
    doc["cs_pin"]    = config.ethCsPin;
    doc["rst_pin"]   = config.ethRstPin;
    doc["int_pin"]   = config.ethIntPin;

    // ETH-specific static IP
    doc["eth_dhcp"] = !config.ethUseStaticIP;
    doc["eth_ip"]   = config.ethIpAddr;
    doc["eth_mask"] = config.ethSubnetMask;
    doc["eth_gw"]   = config.ethGateway;
    doc["eth_dns"]  = config.ethDns;

    String out;
    serializeJson(doc, out);
    apiJson(out);
}

static void apiEthPost() {
    if (!server.hasArg("plain"))
        return apiError(400, F("Missing body"));

    StaticJsonDocument<384> req;
    if (deserializeJson(req, server.arg("plain")))
        return apiError(400, F("Invalid JSON"));

    config.useEthernet    = req["enabled"]  | false;
    config.ethPhyAddr     = (int8_t)(req["phy_addr"] | 1);
    config.ethMisoPin     = (int8_t)(req["miso_pin"] | 12);
    config.ethMosiPin     = (int8_t)(req["mosi_pin"] | 11);
    config.ethSclkPin     = (int8_t)(req["sclk_pin"] | 13);
    config.ethCsPin       = (int8_t)(req["cs_pin"]   | 14);
    config.ethRstPin      = (int8_t)(req["rst_pin"]  | 9);
    config.ethIntPin      = (int8_t)(req["int_pin"]  | 10);

    config.ethUseStaticIP = !(req["eth_dhcp"] | true);
    config.ethIpAddr      = req["eth_ip"]   | "";
    config.ethSubnetMask  = req["eth_mask"] | "";
    config.ethGateway     = req["eth_gw"]   | "";
    config.ethDns         = req["eth_dns"]  | "";

    config.save();
    apiText(F("ETH config saved – reboot to apply"));
}
