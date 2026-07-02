#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <time.h>
#include <esp_heap_caps.h>

#include "py_wifimanager.h"
#include "config.h"
#include "py_log.h"

using namespace WiFiManagerModule;

// Skip ArduinoOTA/mDNS UDP polling when internal DRAM is critically low. The
// lwIP/UDP parsePacket path allocates from internal heap; running it under heap
// pressure can trip a network-stack PANIC (same failure mode guarded for the
// web server via WEB_MIN_FREE_HEAP). Skipping a cycle is harmless: OTA retries,
// and the web /api/ota upload path is unaffected (ArduinoOTA = espota port 3232).
static constexpr size_t OTA_MIN_FREE_HEAP = 14000;

// ----------------------------------------------------
// Internal state
// ----------------------------------------------------
static Preferences prefs;

static bool apActive          = false;
static bool staConnecting     = false;
static unsigned long staStartTime = 0;

static unsigned long apDisableTimer    = 0;
static unsigned long apDisableDuration = 0;

static unsigned long staLostTime = 0;
static unsigned long lastRetry   = 0;

// NTP / time handling
static bool ntpInitialSynced     = false;
static unsigned long lastNtpResyncMillis = 0;

// Periodic AP self-check
static unsigned long lastAPCheck = 0;

// ----------------------------------------------------
// Helpers
// ----------------------------------------------------

// Check if STA has a valid IP
static bool hasValidIP() {
    return WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

// Start Access Point
static void startAP() {
    if (apActive) return;

    Log(LOG_INFO, "WiFiManager: starting AP");
    WiFi.softAP(config.apSSID.c_str(), config.apPass.c_str());
    apActive = true;

    Log(LOG_INFO, "WiFiManager: AP IP = " + WiFi.softAPIP().toString());
}

// Stop Access Point
static void stopAP() {
    if (!apActive) return;

    Log(LOG_INFO, "WiFiManager: stopping AP");
    WiFi.softAPdisconnect(false);
    apActive = false;
}

// Start STA with given credentials
static void startSTA(const String& ssid, const String& pass) {
    Log(LOG_INFO, "WiFiManager: STA connecting to " + ssid);
    staConnecting = true;
    staStartTime  = millis();
    WiFi.begin(ssid.c_str(), pass.c_str());
}

// Load STA credentials from NVS or config and start STA
static void startSTAFromPrefsOrSecrets() {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", config.wifiSSID);
    String pass = prefs.getString("pass", config.wifiPass);
    prefs.end();

    if (ssid.length() == 0 || pass.length() < 8) {
        Log(LOG_WARN, "WiFiManager: no valid credentials → AP only");
        startAP();
        staConnecting = false;
        return;
    }

    startSTA(ssid, pass);
}


WifiStatus WiFiManagerModule::getStatus() {
    WifiStatus s;

    bool sta = (WiFi.status() == WL_CONNECTED && hasValidIP());

    if (sta) {
        s.mode = "STA";
        s.connected = true;
    } else if (apActive) {
        s.mode = "AP";
        s.connected = false;
    } else {
        s.mode = "none";
        s.connected = false;
    }

    s.ssid = WiFi.SSID();
    s.rssi = WiFi.RSSI();
    s.ip   = WiFi.localIP().toString();
    s.mac  = WiFi.macAddress();

    return s;
}

// Map IANA timezone + DST flag to TZ string
// NOTE: extend this table as needed
static void applyTimezoneFromConfig() {
    String tzString = findPosixForTimezone(config.timezone);
    setenv("TZ", tzString.c_str(), 1);
    tzset();
    Log(LOG_INFO, "WiFiManager: timezone set to " + config.timezone +
                  " (TZ=" + tzString + ")");
}


// Trigger NTP sync via configTime (non-blocking)
static void triggerNtpSync() {

    // Manual time mode → no NTP
    if (config.manual_mode) {
        Log(LOG_INFO, "WiFiManager: manual time mode → skipping NTP sync");
        return;
    }

    // Gateway as NTP server
    if (config.use_gateway_ntp) {
        IPAddress gw = WiFi.gatewayIP();
        if (gw != IPAddress(0,0,0,0)) {
            config.ntpServer = gw.toString();
            Log(LOG_INFO, "WiFiManager: using gateway as NTP server: " + config.ntpServer);
        } else {
            Log(LOG_WARN, "WiFiManager: gateway not available → fallback to pool.ntp.org");
            config.ntpServer = "pool.ntp.org";
        }
    }

    // Manual NTP server
    else if (config.manual_ntp) {
        Log(LOG_INFO, "WiFiManager: using manual NTP server: " + config.ntpServer);
    }

    // Default NTP server
    else {
        config.ntpServer = "pool.ntp.org";
        Log(LOG_INFO, "WiFiManager: using default NTP server: pool.ntp.org");
    }

    applyTimezoneFromConfig();
    configTime(0, 0, config.ntpServer.c_str());
    lastNtpResyncMillis = millis();

    Log(LOG_INFO, "WiFiManager: NTP sync requested, server=" + config.ntpServer);
}

// Handle NTP initial sync + periodic resync
static void handleNtpLogic() {
    // Initial sync: wait until system time becomes valid
    if (!ntpInitialSynced && config.isSystemTimeValid()) {
        ntpInitialSynced = true;

        // Jetzt erst TZ anwenden
        applyTimezoneFromConfig();

        // Zeit erneut holen (jetzt mit DST)
        String t = config.getCurrentTimeString();
        config.currentTime = t;
        config.saveSystemConfig();  // Nur System-Config speichern, KEINE Field-Maps (zu heap-intensiv)

        Log(LOG_INFO, "WiFiManager: NTP initial sync → " + t);
    }

    // Periodic resync every ntpResyncInterval seconds
    if (ntpInitialSynced &&
        (millis() - lastNtpResyncMillis > config.ntpResyncInterval * 1000UL)) {

        Log(LOG_INFO, "WiFiManager: NTP periodic resync");
        triggerNtpSync();
    }
}

// ----------------------------------------------------
// Public API
// ----------------------------------------------------
void WiFiManagerModule::begin() {
    Log(LOG_INFO, "WiFiManager: init dual mode (WIFI_AP_STA)");
    WiFi.mode(WIFI_AP_STA);
    delay(100);

    // Make sure AP is OFF at start, we control it manually
    WiFi.softAPdisconnect(true);
    apActive = false;
    Log(LOG_INFO, "WiFiManager: forced AP OFF at startup");

    // Start STA from stored credentials or config
    startSTAFromPrefsOrSecrets();
}

void WiFiManagerModule::loop() {

    // ----------------------------------------------------
    // Monitor STA connection attempts
    // ----------------------------------------------------
    if (staConnecting) {
        wl_status_t st = WiFi.status();

        if (st == WL_CONNECTED && hasValidIP()) {
            Log(LOG_INFO, "WiFiManager: STA connected, IP=" + WiFi.localIP().toString());
            staConnecting = false;
            staLostTime   = 0;

            // If AP is active, schedule shutdown in 90s
            if (apActive) {
                apDisableTimer    = millis();
                apDisableDuration = 90000;
                Log(LOG_INFO, "WiFiManager: AP will stop in 90s (STA connected)");
            }

            // Start OTA once (shared with EthManager via config.otaStarted)
            if (!config.otaStarted) {
                config.otaStarted = true;
                ArduinoOTA.setHostname(config.hostname.c_str());
                if (MDNS.begin(config.hostname.c_str())) {
                    Log(LOG_INFO, "WiFiManager: mDNS started: " + config.hostname);
                } else {
                    Log(LOG_WARN, "WiFiManager: mDNS start failed");
                }
                ArduinoOTA.begin();
                Log(LOG_INFO, "WiFiManager: OTA ready");
            }

            // Trigger NTP sync on successful STA connect
            triggerNtpSync();
        }

        // Connection timeout → AP fallback
        if (millis() - staStartTime > 15000 && st != WL_CONNECTED) {
            Log(LOG_WARN, "WiFiManager: STA connect timeout → AP fallback");
            staConnecting = false;
            startAP();
        }
    }

    // ----------------------------------------------------
    // STA lost → AP after 15s, periodic retry
    // ----------------------------------------------------
    if (!staConnecting && WiFi.status() != WL_CONNECTED) {

        if (staLostTime == 0)
            staLostTime = millis();

        // After 15s without STA → AP fallback
        if (!apActive && millis() - staLostTime > 15000) {
            Log(LOG_WARN, "WiFiManager: STA lost >15s → AP fallback");
            startAP();
            apDisableDuration = 0; // keep AP until STA is back
        }

        // Retry STA every 30s
        if (millis() - lastRetry > 30000) {
            lastRetry = millis();
            Log(LOG_INFO, "WiFiManager: retry STA");
            startSTAFromPrefsOrSecrets();
        }
    }

    // ----------------------------------------------------
    // AP auto-disable after timer
    // ----------------------------------------------------
    if (apActive && apDisableDuration > 0) {
        if (millis() - apDisableTimer > apDisableDuration) {
            Log(LOG_INFO, "WiFiManager: AP timer expired → stop AP");
            stopAP();
            apDisableDuration = 0;
        }
    }

    // ----------------------------------------------------
    // AP self-check every 5 minutes
    // ----------------------------------------------------
    if (millis() - lastAPCheck > 300000) { // 5 minutes
        lastAPCheck = millis();

        bool staConnected = (WiFi.status() == WL_CONNECTED && hasValidIP());

        // If STA is connected and AP is still on → schedule shutdown in 90s
        if (staConnected && apActive) {
            apDisableTimer    = millis();
            apDisableDuration = 90000;
            Log(LOG_INFO, "WiFiManager: AP scheduled to stop in 90s (periodic check)");
        }

        // If STA is not connected and AP is off → turn AP on
        if (!staConnected && !apActive) {
            Log(LOG_WARN, "WiFiManager: STA not connected → AP ON (periodic check)");
            startAP();
        }
    }

    // ----------------------------------------------------
    // OTA + NTP handling
    // ----------------------------------------------------
    if (WiFi.status() == WL_CONNECTED) {
        if (config.otaStarted &&
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= OTA_MIN_FREE_HEAP) {
            ArduinoOTA.handle();
        }
        handleNtpLogic();
    }
}

// ----------------------------------------------------
// Set WLAN credentials and connect
// ----------------------------------------------------
void WiFiManagerModule::connect(const String& ssid, const String& pass) {

    if (ssid.length() == 0 || pass.length() < 8) {
        Log(LOG_WARN, "WiFiManager: invalid credentials → abort");
        return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();

    Log(LOG_INFO, "WiFiManager: new credentials saved: " + ssid);

    startSTA(ssid, pass);
}

// ----------------------------------------------------
// WiFi reset (clear credentials, AP only)
// ----------------------------------------------------
void WiFiManagerModule::resetWiFi() {
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();

    Log(LOG_WARN, "WiFiManager: WiFi reset → AP only");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    delay(100);

    apDisableDuration = 0;
    startAP();
}

// ----------------------------------------------------
// Start temporary AP for given duration
// ----------------------------------------------------
void WiFiManagerModule::startTemporaryAP(unsigned long durationMs) {
    Log(LOG_INFO, "WiFiManager: temporary AP for " + String(durationMs) + " ms");

    startAP();
    apDisableTimer    = millis();
    apDisableDuration = durationMs;
}

// ----------------------------------------------------
// Status JSON for dashboard
// ----------------------------------------------------
String WiFiManagerModule::getStatusJson() {
    DynamicJsonDocument doc(256);

    if (WiFi.status() == WL_CONNECTED && hasValidIP()) {
        doc["mode"] = "STA";
    } else if (apActive) {
        doc["mode"] = "AP";
    } else {
        doc["mode"] = "none";
    }

    doc["connected"] = (WiFi.status() == WL_CONNECTED && hasValidIP());
    doc["ssid"]      = WiFi.SSID();
    doc["rssi"]      = WiFi.RSSI();
    doc["ip"]        = WiFi.localIP().toString();
    doc["mac"]       = WiFi.macAddress();

    String out;
    serializeJson(doc, out);
    return out;
}

// ----------------------------------------------------
// WiFi scan result as JSON
// ----------------------------------------------------
String WiFiManagerModule::scanJson() {
    int n = WiFi.scanNetworks();
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("nets");

    for (int i = 0; i < n; i++) {
        JsonObject o = arr.createNestedObject();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["enc"]  = WiFi.encryptionType(i);
    }

    String out;
    serializeJson(doc, out);
    return out;
}

void WiFiManagerModule::setManualTime(int year, int month, int day, int hour, int minute, bool dst) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour + (dst ? 1 : 0);
    t.tm_min  = minute;
    t.tm_sec  = 0;

    time_t ts = mktime(&t);
    struct timeval now = { .tv_sec = ts, .tv_usec = 0 };
    settimeofday(&now, nullptr);

    ntpInitialSynced = true;

    applyTimezoneFromConfig();

    String cur = config.getCurrentTimeString();
    config.currentTime = cur;
    config.save();

    Log(LOG_WARN, "WiFiManager: manual time set → " + cur + " (DST=" + String(dst ? "on" : "off") + ")");
}

// Apply current timezone from config to system
void WiFiManagerModule::applyTimezone() {
    applyTimezoneFromConfig();
}