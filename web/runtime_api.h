#pragma once
#include <WebServer.h>
#include "../py_log.h"
#include "../config.h"

extern WebServer server;

inline void registerRuntimeAPI() {

    // /api/log
    server.on("/api/log", HTTP_GET, []() {
        server.send(200, "text/plain", WebLogGet());
    });

    // /api/log/level (GET)
    server.on("/api/log/level", HTTP_GET, []() {
        DynamicJsonDocument doc(256);
        doc["info"]  = config.logInfo;
        doc["warn"]  = config.logWarn;
        doc["error"] = config.logError;
        doc["debug"] = config.logDebug;
        doc["syslogEnabled"] = config.syslogEnabled;
        doc["syslogServer"]  = config.syslogServer;
        doc["syslogPort"]    = config.syslogPort;

        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    // /api/log/level (POST)
    server.on("/api/log/level", HTTP_POST, []() {

        if (!server.hasArg("plain")) {
            server.send(400, "text/plain", "Missing body");
            return;
        }

        DynamicJsonDocument req(256);
        if (deserializeJson(req, server.arg("plain"))) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }

        if (req.containsKey("info"))  config.logInfo  = req["info"];
        if (req.containsKey("warn"))  config.logWarn  = req["warn"];
        if (req.containsKey("error")) config.logError = req["error"];
        if (req.containsKey("debug")) config.logDebug = req["debug"];

        if (req.containsKey("syslogEnabled")) config.syslogEnabled = req["syslogEnabled"];
        if (req.containsKey("syslogServer"))  config.syslogServer  = req["syslogServer"].as<String>();
        if (req.containsKey("syslogPort"))    config.syslogPort    = req["syslogPort"];

        // Sichern
        config.saveSystemConfig();
        
        // Sofort verifizieren dass die Werte korrekt gespeichert wurden
        bool enabled_stored = config.syslogEnabled;
        String server_stored = config.syslogServer;
        uint16_t port_stored = config.syslogPort;
        
        // Aus NVS direkt erneut laden zur Verifikation
        Preferences p;
        p.begin("config", true);  // read-only
        bool enabled_nv = p.getBool("syslog_en", false);
        String server_nv = p.getString("syslog_srv", "");
        uint16_t port_nv = p.getUShort("syslog_port", 514);
        p.end();
        
        // Debug-Log
        if (enabled_nv == enabled_stored && server_nv == server_stored && port_nv == port_stored) {
            Log(LOG_INFO, 
                String("Syslog persisted OK: enabled=") + 
                (enabled_nv ? "true" : "false") +
                " server=" + server_nv + 
                " port=" + String(port_nv));
        } else {
            Log(LOG_ERROR,
                String("Syslog persist MISMATCH! stored: en=") + (enabled_stored ? "1" : "0") +
                " srv=" + server_stored + " port=" + port_stored +
                " vs NVS: en=" + (enabled_nv ? "1" : "0") + " srv=" + server_nv + 
                " port=" + port_nv);
        }
        
        server.send(200, "text/plain", "Log level updated");
    });
}
