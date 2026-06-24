#pragma once
#include <WebServer.h>
#include <WiFi.h>

// KORREKTE Pfade aus dem Unterordner:
#include "../py_mqtt.h"
#include "../py_parser_pwr.h"
#include "../py_wifimanager.h"
#include "../py_eth.h"

extern AppConfig config;
extern PyMqtt py_mqtt;
extern BatteryStack lastParsedStack;

void registerDashboardAPI(WebServer &server) {

    server.on("/api/dashboard", HTTP_GET, [&]() {

        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "application/json", "");
        server.sendContent("{");

        // WiFi
        server.sendContent("\"wifi\":{");
        server.sendContent("\"mode\":\"STA\",");
        server.sendContent("\"ssid\":\"" + WiFi.SSID() + "\",");
        server.sendContent("\"ip\":\"" + WiFi.localIP().toString() + "\",");
        server.sendContent("\"mac\":\"" + WiFi.macAddress() + "\",");
        server.sendContent("\"rssi\":" + String(WiFi.RSSI()));
        server.sendContent("},");

        // LAN (Ethernet)
        EthStatus ethStatus = EthManagerModule::getStatus();
        
        server.sendContent("\"lan\":{");
        server.sendContent("\"enabled\":" + String(config.useEthernet ? "true" : "false") + ",");
        server.sendContent("\"linked\":" + String(ethStatus.linked ? "true" : "false") + ",");
        server.sendContent("\"connected\":" + String(ethStatus.connected ? "true" : "false") + ",");
        server.sendContent("\"ip\":\"" + ethStatus.ip + "\",");
        server.sendContent("\"mac\":\"" + ethStatus.mac + "\",");
        server.sendContent("\"speed\":" + String(ethStatus.speed));
        server.sendContent("},");

        // MQTT
        server.sendContent("\"mqtt\":{");
        server.sendContent("\"connected\":" + String(py_mqtt.isConnected() ? "true" : "false") + ",");
        server.sendContent("\"server\":\"" + config.mqtt.server + "\",");
        server.sendContent("\"port\":" + String(config.mqtt.port) + ",");
        server.sendContent("\"last_contact\":\"" + config.lastMqttContact + "\"");
        server.sendContent("},");

        // Battery
        server.sendContent("\"battery\":{");
        server.sendContent("\"modules\":" + String(lastParsedStack.batteryCount) + ",");
        server.sendContent("\"soc\":" + String(lastParsedStack.soc) + ",");
        server.sendContent("\"last_update\":\"" + config.lastPwrUpdate + "\"");
        server.sendContent("},");

        // Stack Status (computed from lastParsedStack)
        float stackVolt = lastParsedStack.avgVoltage_mV / 1000.0f;
        float stackCurr = lastParsedStack.totalCurrent_mA / 1000.0f;
        float stackPower = stackCurr * stackVolt;
        float stackPowerIn = (lastParsedStack.totalCurrent_mA > 0) ? stackPower : 0.0f;
        float stackPowerOut = (lastParsedStack.totalCurrent_mA < 0) ? -stackPower : 0.0f;
        
        server.sendContent("\"stack\":{");
        server.sendContent("\"voltage\":" + String(stackVolt, 2) + ",");
        server.sendContent("\"current\":" + String(stackCurr, 2) + ",");
        server.sendContent("\"power\":" + String(stackPower, 1) + ",");
        server.sendContent("\"power_in\":" + String(stackPowerIn, 1) + ",");
        server.sendContent("\"power_out\":" + String(stackPowerOut, 1) + ",");
        server.sendContent("\"temperature\":" + String(lastParsedStack.temperature));
        server.sendContent("},");

        // System (calculate local time directly using offset)
        time_t now;
        time(&now);
        
        // Get UTC time
        struct tm utc_t;
        gmtime_r(&now, &utc_t);
        
        // Calculate local time by applying offset
        int offsetHours = config.getTimezoneOffsetHours();
        time_t local_time = now + (offsetHours * 3600);
        struct tm t;
        gmtime_r(&local_time, &t);
        
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        char utc_buf[32];
        strftime(utc_buf, sizeof(utc_buf), "%Y-%m-%d %H:%M:%S", &utc_t);

        server.sendContent("\"system\":{" );
        server.sendContent("\"time\":\"" + String(buf) + "\",");
        server.sendContent("\"timestamp\":" + String(now) + ",");
        server.sendContent("\"utc\":\"" + String(utc_buf) + "\",");
        server.sendContent("\"offset\":" + String(offsetHours) + ",");
        server.sendContent("\"timezone\":\"" + config.timezone + "\",");
        server.sendContent("\"uptime\":\"" + config.uptimeString() + "\",");
        server.sendContent("\"version\":\"" + config.firmwareVersion + "\"");
        server.sendContent("}");

        server.sendContent("}");
        server.sendContent("");
    });
}
