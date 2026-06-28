#include "config.h"
#include "py_log.h"
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <cstring>
#include <esp_app_desc.h>



AppConfig config;

static int monthFromBuildDate(const char* mon) {
    if (strcmp(mon, "Jan") == 0) return 1;
    if (strcmp(mon, "Feb") == 0) return 2;
    if (strcmp(mon, "Mar") == 0) return 3;
    if (strcmp(mon, "Apr") == 0) return 4;
    if (strcmp(mon, "May") == 0) return 5;
    if (strcmp(mon, "Jun") == 0) return 6;
    if (strcmp(mon, "Jul") == 0) return 7;
    if (strcmp(mon, "Aug") == 0) return 8;
    if (strcmp(mon, "Sep") == 0) return 9;
    if (strcmp(mon, "Oct") == 0) return 10;
    if (strcmp(mon, "Nov") == 0) return 11;
    if (strcmp(mon, "Dec") == 0) return 12;
    return 1;
}

String AppConfig::getBuildNumber() const {
    // Use image ELF SHA from running firmware metadata.
    // This changes whenever the compiled firmware image changes and avoids
    // stale values caused by incremental compilation of a single source file.
    const esp_app_desc_t* app = esp_app_get_description();
    if (app) {
        bool anyNonZero = false;
        for (int i = 0; i < 32; ++i) {
            if (app->app_elf_sha256[i] != 0) {
                anyNonZero = true;
                break;
            }
        }

        if (anyNonZero) {
            // 12 hex chars from first 6 bytes keep the UI short and stable.
            char buf[13];
            for (int i = 0; i < 6; ++i) {
                snprintf(&buf[i * 2], 3, "%02x", (unsigned int)app->app_elf_sha256[i]);
            }
            buf[12] = '\0';
            return String(buf);
        }
    }

    // Fallback: compile timestamp if app metadata is unavailable.
    const char* d = __DATE__; // "Mmm dd yyyy"
    const char* t = __TIME__; // "hh:mm:ss"

    char mon[4] = { d[0], d[1], d[2], '\0' };
    int month = monthFromBuildDate(mon);

    int day = ((d[4] == ' ') ? 0 : (d[4] - '0')) * 10 + (d[5] - '0');
    int year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');

    int hour = (t[0] - '0') * 10 + (t[1] - '0');
    int min  = (t[3] - '0') * 10 + (t[4] - '0');
    int sec  = (t[6] - '0') * 10 + (t[7] - '0');

    char buf[20];
    snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", year, month, day, hour, min, sec);
    return String(buf);
}

String AppConfig::getFirmwareVersionWithBuild() const {
    return firmwareVersion + "+b" + getBuildNumber();
}

// Check if a given date is within DST period (European rule: last Sunday of March to last Sunday of October)
// Returns offset considering DST
int AppConfig::getTimezoneOffsetHours() const {
    String posix = findPosixForTimezone(timezone);

    // POSIX sign is INVERTED vs real UTC offset:
    //   "CET-1" = UTC+1,  "EST+5" = UTC-5,  "GMT0" = UTC+0
    int stdOffset = 0;
    bool posixMinus = false;

    for (int i = 0; i < (int)posix.length(); i++) {
        char c = posix[i];
        if (isdigit(c)) {
            stdOffset = c - '0';
            if (i > 0 && posix[i-1] == '-') posixMinus = true;
            break;
        }
    }
    if (!posixMinus) stdOffset = -stdOffset;  // flip: bare/+ → negative real offset

    // Automatic DST: if POSIX string has transition rules (contains ','), apply +1h in summer
    // Does NOT require manual_dst – works for NTP mode automatically
    if (posix.indexOf(',') >= 0) {
        time_t now = time(NULL);
        struct tm t;
        gmtime_r(&now, &t);
        int month = t.tm_mon + 1;
        int day   = t.tm_mday;
        // European DST approx: March 25 – October 24
        bool inDst = (month > 3 && month < 10) ||
                     (month == 3  && day >= 25) ||
                     (month == 10 && day <= 24);
        if (inDst) return stdOffset + 1;
    }

    return stdOffset;
}


static const size_t CHUNK_SIZE = 1500;
static const int MAX_JSON_CHUNKS = 80;  // Reduziert um Overflow zu vermeiden

static const size_t DOC_CAP_PW_FIELDS = 16384;
static const size_t DOC_CAP_BAT_FIELDS = 24576;
static const size_t DOC_CAP_STAT_FIELDS = 32768;
static const size_t DOC_CAP_INFO_FIELDS = 32768;

static size_t jsonDocCapacityForPayload(size_t payloadLen,
                                        size_t minCap,
                                        size_t maxCap) {
    size_t cap = payloadLen + 4096;
    if (cap < minCap) cap = minCap;
    if (cap > maxCap) cap = maxCap;
    return cap;
}

static unsigned long clampIntervalMs(unsigned long value,
                                     unsigned long minMs,
                                     unsigned long maxMs,
                                     unsigned long fallbackMs,
                                     const char* label) {
    if (value < minMs || value > maxMs) {
        Log(LOG_WARN,
            String("Config: ") + label + " interval out of range (" +
            String(value) + " ms), using " + String(fallbackMs) + " ms");
        return fallbackMs;
    }
    return value;
}

HealthStatus health;

static void ensureDefaultPwrFields(std::map<String, FieldConfig>& fields) {
    if (!fields.empty()) return;

    auto add = [&](const String& key,
                   const String& label,
                   const String& factor,
                   const String& unit,
                   bool active) {
        FieldConfig f;
        f.label = label;
        f.display = label;
        f.factor = factor;
        f.unit = unit;
        f.mqtt = active;
        f.send = active;
        fields[key] = f;
    };

    add("Volt",    "Voltage",     "0.001", "V",  true);
    add("Curr",    "Current",     "0.001", "A",  true);
    add("Tempr",   "Temperature", "0.001", "°C", true);
    add("Coulomb", "SOC",         "1",     "%",  true);
}


// ----------------------------------------------------
//  Helper: Save JSON in chunks
// ----------------------------------------------------
void AppConfig::saveJsonChunked(const char* ns, const char* prefix, const String& json) {

    Log(LOG_INFO, String("NVS-CHUNK: Saving JSON for namespace '") + ns + "', prefix '" + prefix + "'");
    Log(LOG_INFO, String("NVS-CHUNK: JSON length = ") + json.length());

    size_t len = json.length();

    // Sicherheitsnetz: ein leerer Payload darf NIE bestehende Chunks loeschen.
    // (Sonst wuerde z.B. ein versehentlich leeres fieldsStat die NVS-Daten vernichten.)
    if (len == 0) {
        Log(LOG_WARN, String("NVS-CHUNK: empty payload for prefix '") + prefix + "' - skipping save (existing data preserved)");
        return;
    }

    size_t neededChunks = (len + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (neededChunks > (size_t)MAX_JSON_CHUNKS) {
        Log(LOG_ERROR, String("NVS-CHUNK: payload too large for chunk limit, needed=") + neededChunks + ", max=" + MAX_JSON_CHUNKS);
        return;
    }

    Preferences p;
    p.begin(ns, false);

    // WICHTIG: Neue Chunks ZUERST schreiben, alte NICHT vorab loeschen.
    // Schlaegt ein Schreibvorgang fehl (z.B. NVS voll), bleiben die bestehenden
    // Daten erhalten statt komplett geloescht zu werden.
    int index = 0;
    int written = 0;
    bool writeOk = true;

    while ((size_t)index * CHUNK_SIZE < len) {

        size_t start = (size_t)index * CHUNK_SIZE;
        size_t end   = start + CHUNK_SIZE;
        if (end > len) end = len;

        String part;
        part.reserve(end - start);
        for (size_t i = start; i < end; i++) {
            part += json[i];
        }

        String key = String(prefix) + "_" + index;

        bool ok = p.putString(key.c_str(), part);
        if (!ok) {
            Log(LOG_ERROR, String("NVS-CHUNK: FAILED writing chunk #") + index + " - aborting, existing data left untouched where possible");
            writeOk = false;
            break;
        }

        index++;
        written++;
    }

    if (!writeOk) {
        // Schreibfehler: ueberzaehlige alte Chunks NICHT entfernen.
        // Der Load-Pfad validiert das JSON und behaelt bei Fehler die Vorkonfiguration.
        p.end();
        return;
    }

    // Erst nach erfolgreichem Schreiben aller Chunks ueberzaehlige alte Chunks entfernen.
    for (int i = index; i < MAX_JSON_CHUNKS; i++) {
        String key = String(prefix) + "_" + i;
        if (p.isKey(key.c_str())) {
            p.remove(key.c_str());
        }
    }

    Log(LOG_INFO, String("NVS-CHUNK: Total chunks written = ") + written);

    p.end();
}
// ----------------------------------------------------
//  Helper: Load JSON from chunks
// ----------------------------------------------------
String AppConfig::loadJsonChunked(const char* ns, const char* prefix) {
    Preferences p;
    p.begin(ns, true);

    String json = "";
    for (int i = 0; i < MAX_JSON_CHUNKS; i++) {
        String key = String(prefix) + "_" + String(i);
        if (!p.isKey(key.c_str())) break;
        json += p.getString(key.c_str(), "");
    }

    p.end();
    return json;
}

// ----------------------------------------------------
//  Generate hostname from MAC address
// ----------------------------------------------------
String AppConfig::generateHostname() {
    uint64_t mac = ESP.getEfuseMac();
    uint16_t last = mac & 0xFFFF;

    char buf[32];
    snprintf(buf, sizeof(buf), "pylontech-%04X", last);
    return String(buf);
}

// ----------------------------------------------------
//  Generate Time
// ----------------------------------------------------
String AppConfig::getCurrentTimeString() {
    time_t now;
    time(&now);

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    return String(buf);
}

bool AppConfig::isSystemTimeValid() {
    time_t now;
    time(&now);
    return (now > 1700000000); // > 2023-01-01
}

// ----------------------------------------------------
//  NVS komplett löschen
// ----------------------------------------------------
void AppConfig::clearNVS() {
    Preferences p;
    p.begin("config", false);
    p.clear();
    p.end();

    p.begin("battery_pwr", false);
    p.clear();
    p.end();

    p.begin("battery_bat", false);
    p.clear();
    p.end();

    p.begin("battery_stat", false);
    p.clear();
    p.end();

    Log(LOG_WARN, "NVS cleared");
}

// ----------------------------------------------------
//  Default-Werte setzen (ohne löschen, ohne reboot)
// ----------------------------------------------------
void AppConfig::factoryDefaults() {

    // Battery intervals
    battery.intervalPwr  = 60000;
    battery.intervalBat  = 300000;
    battery.intervalStat = 1800000;

    battery.enableBat  = true;
    battery.enableStat = true;
    battery.useFahrenheit = false;

    // MQTT defaults
    mqtt.enabled = false;
    mqtt.server  = "";
    mqtt.port    = 1883;
    mqtt.user    = "";
    mqtt.pass    = "";

    mqtt.prefix     = "Pylontech";
    mqtt.topicStack = "Stack";
    mqtt.topicPwr   = "pwr";
    mqtt.topicBat   = "bat";
    mqtt.topicStat  = "stat";
    mqtt.mode       = "active";

    // Syslog defaults
    syslogEnabled = false;
    syslogServer  = "";
    syslogPort    = 514;

    // Hostname / AP
    hostname = generateHostname();
    apSSID   = hostname;

    // NTP
    ntpServer = "pool.ntp.org";
    // Manual time mode
    manual_mode = false;
    manual_date = "";
    manual_time = "";
    manual_dst  = false;

    use_gateway_ntp = true;   // factory default
    manual_ntp      = false;

    // Default PWR fields
    battery.fieldsPwr.clear();

    auto add = [&](std::map<String, FieldConfig>& map, String key, String label, String factor, String unit, bool active){
        FieldConfig f;
            f.label = label;
            f.display = label;   // NEW: default display name = label
            f.factor = factor;
            f.unit = unit;
            f.mqtt = active;
            f.send = active;

            map[key] = f;

    };

    add(battery.fieldsPwr, "Volt",    "Voltage",     "0.001", "V",  true);
    add(battery.fieldsPwr, "Curr",    "Current",     "0.001", "A",  true);
    add(battery.fieldsPwr, "Tempr",   "Temperature", "0.001", "°C", true);
    add(battery.fieldsPwr, "Coulomb", "SOC",         "1",     "%",  true);

    // BAT + STAT empty by default
    battery.fieldsBat.clear();
    battery.fieldsStat.clear();

    //firmwareVersion = "1.1.0";
    currentTime     = "";
    lastPwrUpdate   = "";
    detectedModules = 0;
    lastMqttContact = "";

    Log(LOG_INFO, "factoryDefaults(): default fields set");
}

// ----------------------------------------------------
//  Factory Reset (löscht ALLES + reboot)
// ----------------------------------------------------
void AppConfig::factoryReset() {
    Log(LOG_WARN, "Factory reset triggered");
    clearNVS();
    delay(200);
    ESP.restart();
}

// ----------------------------------------------------
//  Load ONLY system configuration
// ----------------------------------------------------
void AppConfig::loadSystemConfig() {
    Preferences p;
    p.begin("config", true);

    deviceName = p.getString("devname", deviceName);
    hostname   = p.getString("hostname", hostname);
    wifiSSID   = p.getString("wifi_ssid", wifiSSID);
    wifiPass   = p.getString("wifi_pass", wifiPass);
    apSSID     = p.getString("ap_ssid", apSSID);
    apPass     = p.getString("ap_pass", apPass);
    setupDone  = p.getBool("setup", setupDone);

    useStaticIP = p.getBool("net_static", useStaticIP);
    ipAddr      = p.getString("net_ip", ipAddr);
    subnetMask  = p.getString("net_mask", subnetMask);
    gateway     = p.getString("net_gw", gateway);
    dns         = p.getString("net_dns", dns);

    useEthernet    = p.getBool("eth_en",    useEthernet);
    ethPhyAddr      = (int8_t)p.getChar("eth_phy",   ethPhyAddr);
    ethMisoPin      = (int8_t)p.getChar("eth_miso",  ethMisoPin);
    ethMosiPin      = (int8_t)p.getChar("eth_mosi",  ethMosiPin);
    ethSclkPin      = (int8_t)p.getChar("eth_sclk",  ethSclkPin);
    ethCsPin        = (int8_t)p.getChar("eth_cs",    ethCsPin);
    ethRstPin       = (int8_t)p.getChar("eth_rst",   ethRstPin);
    ethIntPin       = (int8_t)p.getChar("eth_int",   ethIntPin);
    ethUseStaticIP  = p.getBool("eth_static",  ethUseStaticIP);
    ethIpAddr       = p.getString("eth_ip",    ethIpAddr);
    ethSubnetMask   = p.getString("eth_mask",  ethSubnetMask);
    ethGateway      = p.getString("eth_gw",    ethGateway);
    ethDns          = p.getString("eth_dns",   ethDns);

    ntpServer         = p.getString("ntp_srv", ntpServer);
    timezone          = p.getString("tz_name", timezone);
    daylightSaving    = p.getBool("tz_dst", daylightSaving);
    ntpResyncInterval = p.getULong("ntp_resync", ntpResyncInterval);
    // Manual time mode
    manual_mode = p.getBool("manual_mode", false);
    manual_date = p.getString("manual_date", "");
    manual_time = p.getString("manual_time", "");
    manual_dst  = p.getBool("manual_dst", false);

    // NTP modes
    use_gateway_ntp = p.getBool("use_gateway_ntp", true);
    manual_ntp      = p.getBool("manual_ntp", false);

    mqtt.enabled = p.getBool("mqtt_en", mqtt.enabled);
    mqtt.server  = p.getString("mqtt_srv", mqtt.server);
    mqtt.port    = p.getUShort("mqtt_port", mqtt.port);
    mqtt.user    = p.getString("mqtt_user", mqtt.user);
    mqtt.pass    = p.getString("mqtt_pass", mqtt.pass);

    mqtt.prefix     = p.getString("mqtt_prefix", mqtt.prefix);
    mqtt.topicStack = p.getString("mqtt_t_stack", mqtt.topicStack);
    mqtt.topicPwr   = p.getString("mqtt_t_pwr",   mqtt.topicPwr);
    mqtt.topicBat   = p.getString("mqtt_t_bat",   mqtt.topicBat);
    mqtt.topicStat  = p.getString("mqtt_t_stat",  mqtt.topicStat);
    mqtt.mode       = p.getString("mqtt_mode",    mqtt.mode);
    mqtt.cellPrefix = p.getString("mqtt_cellprefix", mqtt.cellPrefix);

    //firmwareVersion = p.getString("fw_ver", firmwareVersion);
    currentTime     = p.getString("cur_time", currentTime);
    lastPwrUpdate   = p.getString("pwr_last", lastPwrUpdate);
    detectedModules = p.getUShort("pwr_mods", detectedModules);
    lastMqttContact = p.getString("mqtt_last", lastMqttContact);

    logInfo  = p.getBool("log_info",  true);
    logWarn  = p.getBool("log_warn",  true);
    logError = p.getBool("log_error", true);
    logDebug = p.getBool("log_debug", false);
    syslogEnabled = p.getBool("syslog_en", syslogEnabled);
    syslogServer  = p.getString("syslog_srv", syslogServer);
    syslogPort    = p.getUShort("syslog_port", syslogPort);

    p.end();

    // ----------------------------------------------------
    // Defaults laden, wenn Config leer ist
    // ----------------------------------------------------
    if (hostname.length() == 0 || apSSID.length() == 0) {
        Log(LOG_WARN, "Config empty → applying factory defaults");
        factoryDefaults();
        setupDone = true;
        saveSystemConfig();
    }
}


// ----------------------------------------------------
//  Save ONLY system configuration
// ----------------------------------------------------
void AppConfig::saveSystemConfig() {
    Preferences p;
    p.begin("config", false);

    p.putString("devname", deviceName);
    p.putString("hostname", hostname);
    p.putString("wifi_ssid", wifiSSID);
    p.putString("wifi_pass", wifiPass);
    p.putString("ap_ssid", apSSID);
    p.putString("ap_pass", apPass);
    p.putBool("setup", setupDone);

    p.putBool("net_static", useStaticIP);
    p.putString("net_ip", ipAddr);
    p.putString("net_mask", subnetMask);
    p.putString("net_gw", gateway);
    p.putString("net_dns", dns);

    p.putBool("eth_en",     useEthernet);
    p.putChar("eth_phy",    ethPhyAddr);
    p.putChar("eth_miso",   ethMisoPin);
    p.putChar("eth_mosi",   ethMosiPin);
    p.putChar("eth_sclk",   ethSclkPin);
    p.putChar("eth_cs",     ethCsPin);
    p.putChar("eth_rst",    ethRstPin);
    p.putChar("eth_int",    ethIntPin);
    p.putBool("eth_static", ethUseStaticIP);
    p.putString("eth_ip",   ethIpAddr);
    p.putString("eth_mask", ethSubnetMask);
    p.putString("eth_gw",   ethGateway);
    p.putString("eth_dns",  ethDns);

    p.putString("ntp_srv", ntpServer);
    p.putString("tz_name", timezone);
    p.putBool("tz_dst", daylightSaving);
    p.putULong("ntp_resync", ntpResyncInterval);
    // Manual time mode
    p.putBool("manual_mode", manual_mode);
    p.putString("manual_date", manual_date);
    p.putString("manual_time", manual_time);
    p.putBool("manual_dst", manual_dst);

    // NTP modes
    p.putBool("use_gateway_ntp", use_gateway_ntp);
    p.putBool("manual_ntp", manual_ntp);

    p.putBool("mqtt_en", mqtt.enabled);
    p.putString("mqtt_srv", mqtt.server);
    p.putUShort("mqtt_port", mqtt.port);
    p.putString("mqtt_user", mqtt.user);
    p.putString("mqtt_pass", mqtt.pass);

    p.putString("mqtt_prefix", mqtt.prefix);
    p.putString("mqtt_t_stack", mqtt.topicStack);
    p.putString("mqtt_t_pwr",   mqtt.topicPwr);
    p.putString("mqtt_t_bat",   mqtt.topicBat);
    p.putString("mqtt_t_stat",  mqtt.topicStat);
    p.putString("mqtt_mode",    mqtt.mode);
    p.putString("mqtt_cellprefix", mqtt.cellPrefix); 

    //p.putString("fw_ver", firmwareVersion);
    p.putString("cur_time", currentTime);
    p.putString("pwr_last", lastPwrUpdate);
    p.putUShort("pwr_mods", detectedModules);
    p.putString("mqtt_last", lastMqttContact);

    p.putBool("log_info",  logInfo);
    p.putBool("log_warn",  logWarn);
    p.putBool("log_error", logError);
    p.putBool("log_debug", logDebug);
    p.putBool("syslog_en", syslogEnabled);
    p.putString("syslog_srv", syslogServer);
    p.putUShort("syslog_port", syslogPort);

    p.end();
}

// ----------------------------------------------------
//  PWR CONFIG
// ----------------------------------------------------
void AppConfig::loadPwrConfig() {
    Preferences p;
    p.begin("battery_pwr", true);

    battery.intervalPwr = p.getULong("interval", battery.intervalPwr);
    battery.enableBat   = p.getBool("enabled", battery.enableBat);
    battery.intervalPwr = clampIntervalMs(
        battery.intervalPwr,
        5000UL,        // 5s
        3600000UL,     // 1h
        60000UL,       // 60s default
        "PWR"
    );

    // NEW: Load thresholds
    battery.cellDiffWarn  = p.getFloat("cellDiffWarn",  battery.cellDiffWarn);
    battery.cellDiffError = p.getFloat("cellDiffError", battery.cellDiffError);

    p.end();
}

void AppConfig::savePwrConfig() {
    Preferences p;
    p.begin("battery_pwr", false);

    battery.intervalPwr = clampIntervalMs(
        battery.intervalPwr,
        5000UL,
        3600000UL,
        60000UL,
        "PWR"
    );
    p.putULong("interval", battery.intervalPwr);
    p.putBool("enabled", battery.enableBat);

    // NEW: Save thresholds
    p.putFloat("cellDiffWarn", battery.cellDiffWarn);
    p.putFloat("cellDiffError", battery.cellDiffError);

    p.end();
}

// ----------------------------------------------------
//  PWR FIELDS (JSON + chunks)
// ----------------------------------------------------
void AppConfig::savePwrFields() {
    if (battery.fieldsPwr.empty()) {
        Log(LOG_WARN, "savePwrFields: fieldsPwr empty - skipping save to avoid wiping stored NVS config");
        return;
    }

    DynamicJsonDocument doc(DOC_CAP_PW_FIELDS);
    JsonArray arr = doc.createNestedArray("fields");

    for (auto &kv : battery.fieldsPwr) {
        const String& name = kv.first;
        const FieldConfig& fc = kv.second;
        JsonObject o = arr.createNestedObject();
        o["name"]    = name;
        o["display"] = fc.display;
        o["factor"]  = fc.factor;
        o["unit"]    = fc.unit;
        o["mqtt"]    = fc.mqtt;
        o["send"]    = fc.send;
    }

    if (doc.overflowed()) {
        Log(LOG_ERROR, "savePwrFields: JSON document overflow, aborting save");
        return;
    }

    String json;
    serializeJson(doc, json);
    saveJsonChunked("battery_pwr", "pwr", json);
}

void AppConfig::loadPwrFields() {
    String json = loadJsonChunked("battery_pwr", "pwr");
    if (json.length() == 0) {
        ensureDefaultPwrFields(battery.fieldsPwr);
        return;
    }
    if (json.length() > 100000) {
        Log(LOG_ERROR, "loadPwrFields: payload too large (" + String(json.length()) + "), keeping existing config");
        ensureDefaultPwrFields(battery.fieldsPwr);
        return;
    }

    size_t docCap = jsonDocCapacityForPayload(json.length(), DOC_CAP_PW_FIELDS, 65536);
    DynamicJsonDocument doc(docCap);
    if (deserializeJson(doc, json)) {
        Log(LOG_ERROR, "loadPwrFields: JSON deserialization failed (len=" + String(json.length()) + ", cap=" + String(docCap) + "), keeping existing config");
        ensureDefaultPwrFields(battery.fieldsPwr);
        return;
    }

    // Primary format: {"fields":[...]}
    JsonArray arr = doc["fields"].as<JsonArray>();
    // Legacy format A: root is array
    if (arr.isNull() && doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
    }
    if (arr.isNull() || arr.size() == 0) {
        // Legacy format B: object map {"Volt":{...}, ...}
        if (doc.is<JsonObject>()) {
            JsonObject root = doc.as<JsonObject>();
            bool loadedLegacyMap = false;

            battery.fieldsPwr.clear();
            for (JsonPair kv : root) {
                const char* k = kv.key().c_str();
                if (!k || strlen(k) == 0 || strcmp(k, "fields") == 0) continue;
                if (!kv.value().is<JsonObject>()) continue;

                JsonObject o = kv.value().as<JsonObject>();
                FieldConfig fc;
                fc.display = o["display"] | String(k);
                fc.label   = fc.display;
                fc.factor  = o["factor"] | "1";
                fc.unit    = o["unit"] | "";
                fc.mqtt    = o["mqtt"].isNull() ? (o["sendMQTT"] | false) : (o["mqtt"] | false);
                fc.send    = o["send"].isNull() ? (o["sendPayload"] | false) : (o["send"] | false);
                battery.fieldsPwr[String(k)] = fc;
                loadedLegacyMap = true;
            }

            if (loadedLegacyMap) {
                Log(LOG_WARN, "loadPwrFields: loaded legacy object-map format");
                ensureDefaultPwrFields(battery.fieldsPwr);
                return;
            }
        }

        Log(LOG_WARN, "loadPwrFields: No fields array found");
        ensureDefaultPwrFields(battery.fieldsPwr);
        return;
    }

    battery.fieldsPwr.clear();

    for (JsonVariant v : arr) {
        String name;
        String display;
        String factor;
        String unit;
        bool mqtt = false;
        bool send = false;

        if (v.is<JsonObject>()) {
            JsonObject o = v.as<JsonObject>();
            name = o["name"] | "";
            display = o["display"] | name;
            factor = o["factor"] | "1";
            unit = o["unit"] | "";
            mqtt = o["mqtt"].isNull() ? (o["sendMQTT"] | false) : (o["mqtt"] | false);
            send = o["send"].isNull() ? (o["sendPayload"] | false) : (o["send"] | false);
        } else {
            const char* packed = v.as<const char*>();
            if (!packed) continue;

            const char* p = packed;
            size_t packLen = strlen(p);

            const char* s1 = strchr(p, '|');
            if (!s1 || s1 - p < 1) continue;
            const char* s2 = strchr(s1 + 1, '|');
            if (!s2 || s2 <= s1 + 1) continue;
            const char* s3 = strchr(s2 + 1, '|');
            if (!s3 || s3 <= s2 + 1) continue;
            const char* s4 = strchr(s3 + 1, '|');
            if (!s4 || s4 <= s3 + 1) continue;
            const char* s5 = strchr(s4 + 1, '|');
            if (!s5 || s5 <= s4 + 1) continue;

            int p1 = (int)(s1 - p);
            int p2 = (int)(s2 - p);
            int p3 = (int)(s3 - p);
            int p4 = (int)(s4 - p);
            int p5 = (int)(s5 - p);

            if (p5 + 2 > (int)packLen) continue;

            name = String(p, p1);
            display = String(p + p1 + 1, p2 - p1 - 1);
            factor = String(p + p2 + 1, p3 - p2 - 1);
            unit = String(p + p3 + 1, p4 - p3 - 1);
            mqtt = (*(p + p4 + 1) == '1');
            send = (*(p + p5 + 1) == '1');
        }

        if (name.length() == 0) continue;
        if (display.length() == 0) display = name;
        if (factor.length() == 0) factor = "1";

        FieldConfig fc;
        fc.label = display;
        fc.display = display;
        fc.factor = factor;
        fc.unit = unit;
        fc.mqtt = mqtt;
        fc.send = send;

        battery.fieldsPwr[name] = fc;
    }

    ensureDefaultPwrFields(battery.fieldsPwr);
}

void AppConfig::saveBatConfig() {
    Preferences p;
    p.begin("battery_bat", false);

    battery.intervalBat = clampIntervalMs(
        battery.intervalBat,
        10000UL,       // 10s
        7200000UL,     // 2h
        300000UL,      // 5min default
        "BAT"
    );
    p.putULong("interval", battery.intervalBat);
    p.putBool("enabled", battery.enableBat);

    p.end();
}

// ----------------------------------------------------
//  BAT FIELDS (JSON + chunks)
// ----------------------------------------------------
void AppConfig::saveBatFields() {
    if (battery.fieldsBat.empty()) {
        Log(LOG_WARN, "saveBatFields: fieldsBat empty - skipping save to avoid wiping stored NVS config");
        return;
    }

    DynamicJsonDocument doc(DOC_CAP_BAT_FIELDS);
    JsonArray arr = doc.createNestedArray("fields");

    for (auto &kv : battery.fieldsBat) {
        const String& name = kv.first;
        const FieldConfig& fc = kv.second;
        JsonObject o = arr.createNestedObject();
        o["name"]    = name;
        o["display"] = fc.display;
        o["factor"]  = fc.factor;
        o["unit"]    = fc.unit;
        o["mqtt"]    = fc.mqtt;
        o["send"]    = fc.send;
    }

    if (doc.overflowed()) {
        Log(LOG_ERROR, "saveBatFields: JSON document overflow, aborting save");
        return;
    }

    String json;
    serializeJson(doc, json);
    saveJsonChunked("battery_bat", "bat", json);
}

// ----------------------------------------------------
//  BAT CONFIG
// ----------------------------------------------------
void AppConfig::loadBatConfig() {
    Preferences p;
    p.begin("battery_bat", true);

    battery.intervalBat = p.getULong("interval", battery.intervalBat);
    battery.enableBat   = p.getBool("enabled", battery.enableBat);
    battery.intervalBat = clampIntervalMs(
        battery.intervalBat,
        10000UL,
        7200000UL,
        300000UL,
        "BAT"
    );

    p.end();
}

void AppConfig::loadBatFields() {
    String json = loadJsonChunked("battery_bat", "bat");
    if (json.length() == 0) return;
    if (json.length() > 100000) {
        Log(LOG_ERROR, "loadBatFields: payload too large (" + String(json.length()) + "), keeping existing config");
        return;
    }

    size_t docCap = jsonDocCapacityForPayload(json.length(), DOC_CAP_BAT_FIELDS, 65536);
    DynamicJsonDocument doc(docCap);
    if (deserializeJson(doc, json)) {
        Log(LOG_ERROR, "loadBatFields: JSON deserialization failed (len=" + String(json.length()) + ", cap=" + String(docCap) + "), keeping existing config");
        return;
    }

    JsonArray arr = doc["fields"];
    if (arr.isNull() || arr.size() == 0) {
        Log(LOG_WARN, "loadBatFields: No fields array found");
        return;
    }

    battery.fieldsBat.clear();

    for (JsonVariant v : arr) {
        String name;
        String display;
        String factor;
        String unit;
        bool mqtt = false;
        bool send = false;

        if (v.is<JsonObject>()) {
            JsonObject o = v.as<JsonObject>();
            name = o["name"] | "";
            display = o["display"] | name;
            factor = o["factor"] | "1";
            unit = o["unit"] | "";
            mqtt = o["mqtt"].isNull() ? (o["sendMQTT"] | false) : (o["mqtt"] | false);
            send = o["send"].isNull() ? (o["sendPayload"] | false) : (o["send"] | false);
        } else {
            const char* packed = v.as<const char*>();
            if (!packed) continue;

            const char* p = packed;
            size_t packLen = strlen(p);

            const char* s1 = strchr(p, '|');
            if (!s1 || s1 - p < 1) continue;
            const char* s2 = strchr(s1 + 1, '|');
            if (!s2 || s2 <= s1 + 1) continue;
            const char* s3 = strchr(s2 + 1, '|');
            if (!s3 || s3 <= s2 + 1) continue;
            const char* s4 = strchr(s3 + 1, '|');
            if (!s4 || s4 <= s3 + 1) continue;
            const char* s5 = strchr(s4 + 1, '|');
            if (!s5 || s5 <= s4 + 1) continue;

            int p1 = (int)(s1 - p);
            int p2 = (int)(s2 - p);
            int p3 = (int)(s3 - p);
            int p4 = (int)(s4 - p);
            int p5 = (int)(s5 - p);

            if (p5 + 2 > (int)packLen) continue;

            name = String(p, p1);
            display = String(p + p1 + 1, p2 - p1 - 1);
            factor = String(p + p2 + 1, p3 - p2 - 1);
            unit = String(p + p3 + 1, p4 - p3 - 1);
            mqtt = (*(p + p4 + 1) == '1');
            send = (*(p + p5 + 1) == '1');
        }

        if (name.length() == 0) continue;
        if (display.length() == 0) display = name;
        if (factor.length() == 0) factor = "1";

        FieldConfig fc;
        fc.label = display;
        fc.display = display;
        fc.factor = factor;
        fc.unit = unit;
        fc.mqtt = mqtt;
        fc.send = send;

        battery.fieldsBat[name] = fc;
    }
}

// ----------------------------------------------------
//  STAT CONFIG
// ----------------------------------------------------
void AppConfig::loadStatConfig() {
    Preferences p;
    p.begin("battery_stat", true);

    battery.intervalStat = p.getULong("interval", battery.intervalStat);
    battery.enableStat   = p.getBool("enabled", battery.enableStat);
    battery.intervalStat = clampIntervalMs(
        battery.intervalStat,
        10000UL,       // 10s
        7200000UL,     // 2h
        1800000UL,     // 30min default
        "STAT"
    );

    p.end();
}

void AppConfig::saveStatConfig() {
    Preferences p;
    p.begin("battery_stat", false);

    battery.intervalStat = clampIntervalMs(
        battery.intervalStat,
        10000UL,
        7200000UL,
        1800000UL,
        "STAT"
    );
    p.putULong("interval", battery.intervalStat);
    p.putBool("enabled", battery.enableStat);

    p.end();
}

// ----------------------------------------------------
//  INFO CONFIG
// ----------------------------------------------------
void AppConfig::loadInfoConfig() {
    Preferences p;
    p.begin("battery_info", true);

    battery.intervalInfo = p.getULong("interval", battery.intervalInfo);
    battery.enableInfo   = p.getBool("enabled", battery.enableInfo);
    battery.intervalInfo = clampIntervalMs(
        battery.intervalInfo,
        10000UL,       // 10s
        7200000UL,     // 2h
        3600000UL,     // 1h default
        "INFO"
    );

    p.end();
}

void AppConfig::saveInfoConfig() {
    Preferences p;
    p.begin("battery_info", false);

    battery.intervalInfo = clampIntervalMs(
        battery.intervalInfo,
        10000UL,
        7200000UL,
        3600000UL,
        "INFO"
    );
    p.putULong("interval", battery.intervalInfo);
    p.putBool("enabled", battery.enableInfo);

    p.end();
}

// ----------------------------------------------------
//  STAT FIELDS (JSON + chunks)
// ----------------------------------------------------
void AppConfig::saveStatFields() {
    if (battery.fieldsStat.empty()) {
        Log(LOG_WARN, "saveStatFields: fieldsStat empty - skipping save to avoid wiping stored NVS config");
        return;
    }

    DynamicJsonDocument doc(DOC_CAP_STAT_FIELDS);
    JsonArray arr = doc.createNestedArray("fields");

    for (auto &kv : battery.fieldsStat) {
        const String& name = kv.first;
        const FieldConfig& fc = kv.second;
        JsonObject o = arr.createNestedObject();
        o["name"]    = name;
        o["display"] = fc.display;
        o["factor"]  = fc.factor;
        o["unit"]    = fc.unit;
        o["mqtt"]    = fc.mqtt;
        o["send"]    = fc.send;
    }

    if (doc.overflowed()) {
        Log(LOG_ERROR, "saveStatFields: JSON document overflow, aborting save");
        return;
    }

    String json;
    serializeJson(doc, json);
    saveJsonChunked("battery_stat", "stat", json);
}

void AppConfig::loadStatFields() {
    String json = loadJsonChunked("battery_stat", "stat");
    if (json.length() == 0) return;
    if (json.length() > 100000) {
        Log(LOG_ERROR, "loadStatFields: payload too large (" + String(json.length()) + "), keeping existing config");
        return;
    }
    size_t docCap = jsonDocCapacityForPayload(json.length(), DOC_CAP_STAT_FIELDS, 65536);
    DynamicJsonDocument doc(docCap);
    if (deserializeJson(doc, json)) {
        Log(LOG_ERROR, "loadStatFields: JSON deserialization failed (len=" + String(json.length()) + ", cap=" + String(docCap) + "), keeping existing config");
        return;
    }

    JsonArray arr = doc["fields"];
    if (arr.isNull() || arr.size() == 0) {
        Log(LOG_WARN, "loadStatFields: No fields array found");
        return;
    }

    battery.fieldsStat.clear();

    for (JsonVariant v : arr) {
        String name;
        String display;
        String factor;
        String unit;
        bool mqtt = false;
        bool send = false;

        if (v.is<JsonObject>()) {
            JsonObject o = v.as<JsonObject>();
            name = o["name"] | "";
            display = o["display"] | name;
            factor = o["factor"] | "1";
            unit = o["unit"] | "";
            mqtt = o["mqtt"].isNull() ? (o["sendMQTT"] | false) : (o["mqtt"] | false);
            send = o["send"].isNull() ? (o["sendPayload"] | false) : (o["send"] | false);
        } else {
            // Rueckwaertskompatibel: altes packed-Format name|display|factor|unit|mqtt|send
            const char* packed = v.as<const char*>();
            if (!packed) continue;

            const char* p = packed;

            const char* s1 = strchr(p, '|');
            if (!s1) continue;
            const char* s2 = strchr(s1 + 1, '|');
            if (!s2) continue;
            const char* s3 = strchr(s2 + 1, '|');
            if (!s3) continue;
            const char* s4 = strchr(s3 + 1, '|');
            if (!s4) continue;
            const char* s5 = strchr(s4 + 1, '|');
            if (!s5) continue;

            int p1 = (int)(s1 - p);
            int p2 = (int)(s2 - p);
            int p3 = (int)(s3 - p);
            int p4 = (int)(s4 - p);
            int p5 = (int)(s5 - p);

            name = String(p, p1);
            display = String(p + p1 + 1, p2 - p1 - 1);
            factor = String(p + p2 + 1, p3 - p2 - 1);
            unit = String(p + p3 + 1, p4 - p3 - 1);
            mqtt = (*(p + p4 + 1) == '1');
            send = (*(p + p5 + 1) == '1');
        }

        if (name.length() == 0) continue;
        if (display.length() == 0) display = name;
        if (factor.length() == 0) factor = "1";

        FieldConfig fc;
        fc.label = display;
        fc.display = display;
        fc.factor = factor;
        fc.unit = unit;
        fc.mqtt = mqtt;
        fc.send = send;

        battery.fieldsStat[name] = fc;
    }
}

// ----------------------------------------------------
//  INFO FIELDS (JSON + chunks)
// ----------------------------------------------------
void AppConfig::saveInfoFields() {
    if (battery.fieldsInfo.empty()) {
        Log(LOG_WARN, "saveInfoFields: fieldsInfo empty - skipping save to avoid wiping stored NVS config");
        return;
    }

    DynamicJsonDocument doc(DOC_CAP_INFO_FIELDS);
    JsonArray arr = doc.createNestedArray("fields");

    for (auto &kv : battery.fieldsInfo) {
        const String& name = kv.first;
        const FieldConfig& fc = kv.second;
        JsonObject o = arr.createNestedObject();
        o["name"]    = name;
        o["display"] = fc.display;
        o["factor"]  = fc.factor;
        o["unit"]    = fc.unit;
        o["mqtt"]    = fc.mqtt;
        o["send"]    = fc.send;
    }

    if (doc.overflowed()) {
        Log(LOG_ERROR, "saveInfoFields: JSON document overflow, aborting save");
        return;
    }

    String json;
    serializeJson(doc, json);
    saveJsonChunked("battery_info", "info", json);
}

void AppConfig::loadInfoFields() {
    String json = loadJsonChunked("battery_info", "info");
    if (json.length() == 0) return;
    if (json.length() > 100000) {
        Log(LOG_ERROR, "loadInfoFields: payload too large (" + String(json.length()) + "), keeping existing config");
        return;
    }

    size_t docCap = jsonDocCapacityForPayload(json.length(), DOC_CAP_INFO_FIELDS, 65536);
    DynamicJsonDocument doc(docCap);
    if (deserializeJson(doc, json)) {
        Log(LOG_ERROR, "loadInfoFields: JSON deserialization failed (len=" + String(json.length()) + ", cap=" + String(docCap) + "), keeping existing config");
        return;
    }

    JsonArray arr = doc["fields"];
    if (arr.isNull() || arr.size() == 0) {
        Log(LOG_WARN, "loadInfoFields: No fields array found");
        return;
    }

    battery.fieldsInfo.clear();

    for (JsonVariant v : arr) {
        String name;
        String display;
        String factor;
        String unit;
        bool mqtt = false;
        bool send = false;

        if (v.is<JsonObject>()) {
            JsonObject o = v.as<JsonObject>();
            name = o["name"] | "";
            display = o["display"] | name;
            factor = o["factor"] | "1";
            unit = o["unit"] | "";
            mqtt = o["mqtt"].isNull() ? (o["sendMQTT"] | false) : (o["mqtt"] | false);
            send = o["send"].isNull() ? (o["sendPayload"] | false) : (o["send"] | false);
        } else {
            // Rueckwaertskompatibel: altes packed-Format name|display|factor|unit|mqtt|send
            const char* packed = v.as<const char*>();
            if (!packed) continue;

            const char* p = packed;
            size_t packLen = strlen(p);

            const char* s1 = strchr(p, '|');
            if (!s1 || s1 - p < 1) continue;
            const char* s2 = strchr(s1 + 1, '|');
            if (!s2 || s2 <= s1 + 1) continue;
            const char* s3 = strchr(s2 + 1, '|');
            if (!s3 || s3 <= s2 + 1) continue;
            const char* s4 = strchr(s3 + 1, '|');
            if (!s4 || s4 <= s3 + 1) continue;
            const char* s5 = strchr(s4 + 1, '|');
            if (!s5 || s5 <= s4 + 1) continue;

            int p1 = (int)(s1 - p);
            int p2 = (int)(s2 - p);
            int p3 = (int)(s3 - p);
            int p4 = (int)(s4 - p);
            int p5 = (int)(s5 - p);

            if (p5 + 2 > (int)packLen) continue;

            name = String(p, p1);
            display = String(p + p1 + 1, p2 - p1 - 1);
            factor = String(p + p2 + 1, p3 - p2 - 1);
            unit = String(p + p3 + 1, p4 - p3 - 1);
            mqtt = (*(p + p4 + 1) == '1');
            send = (*(p + p5 + 1) == '1');
        }

        if (name.length() == 0) continue;
        if (display.length() == 0) display = name;
        if (factor.length() == 0) factor = "1";

        FieldConfig fc;
        fc.label = display;
        fc.display = display;
        fc.factor = factor;
        fc.unit = unit;
        fc.mqtt = mqtt;
        fc.send = send;

        battery.fieldsInfo[name] = fc;
    }
}

// ----------------------------------------------------
//  Main load() and save()
// ----------------------------------------------------
void AppConfig::load() {
    loadSystemConfig();

    loadPwrConfig();
    loadPwrFields();

    loadBatConfig();
    loadBatFields();

    loadStatConfig();
    loadStatFields();

    loadInfoConfig();
    loadInfoFields();
}

void AppConfig::save() {
    saveSystemConfig();

    savePwrConfig();
    savePwrFields();

    saveBatConfig();
    saveBatFields();

    saveStatConfig();
    saveStatFields();

    saveInfoConfig();
    saveInfoFields();
}


String findPosixForTimezone(const String& tzName) {

    File f = SPIFFS.open("/timezone.json", "r");
    if (!f) {
        Serial.println("ERROR: timezone.json not found");
        return "UTC0";
    }

    // Filter: nur tz + posix laden → DynamicJsonDocument statt 20000 nur noch ~4096 nötig
    StaticJsonDocument<64> filter;
    filter["*"][0]["tz"]    = true;
    filter["*"][0]["posix"] = true;

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
    f.close();

    if (err) {
        Serial.println("ERROR: timezone.json parse failed");
        return "UTC0";
    }

    // Durch alle Regionen iterieren
    for (JsonPair region : doc.as<JsonObject>()) {
        JsonArray arr = region.value().as<JsonArray>();

        for (JsonObject entry : arr) {
            if (entry["tz"].as<String>() == tzName) {
                return entry["posix"].as<String>();
            }
        }
    }

    return "UTC0"; // fallback
}

// ----------------------------------------------------
//  Uptime helper
// ----------------------------------------------------
String AppConfig::uptimeString() {
    unsigned long ms = millis();
    unsigned long s  = ms / 1000;
    unsigned long m  = s / 60;
    unsigned long h  = m / 60;
    unsigned long d  = h / 24;

    char buf[64];
    snprintf(buf, sizeof(buf), "%lu d %02lu:%02lu:%02lu",
             d, h % 24, m % 60, s % 60);

    return String(buf);
}

// Buffer

PwrBuffer pwrA;
PwrBuffer pwrB;
volatile bool pwrUseA = true;

BatBuffer batA;
BatBuffer batB;
volatile bool batUseA = true;

StatBuffer statA;
StatBuffer statB;
volatile bool statUseA = true;

InfoBuffer infoA;
InfoBuffer infoB;
volatile bool infoUseA = true;

// Per-module pending publish buffers
InfoData  g_pendingInfo[MAX_BATTERY_MODULES];
volatile bool g_pendingInfoReady[MAX_BATTERY_MODULES] = {};
StatData  g_pendingStat[MAX_BATTERY_MODULES];
volatile bool g_pendingStatReady[MAX_BATTERY_MODULES] = {};

BatteryMode g_batteryMode = BatteryMode::UNKNOWN;

SemaphoreHandle_t g_statMutex = NULL;
SemaphoreHandle_t g_infoMutex = NULL;
SemaphoreHandle_t g_batMutex  = NULL;
SemaphoreHandle_t g_pwrMutex  = NULL;
SemaphoreHandle_t g_healthMutex = NULL;

