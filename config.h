#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <map>
#include <vector>

// ---------------------------------------------------------
// Timezone entry structure (Region → City → IANA → POSIX)
// ---------------------------------------------------------
struct TimezoneEntry {
    const char* region;
    const char* city;
    const char* tzName;
    const char* posix;
};


String getTimezoneJson();
String findPosixForTimezone(const String& tzName);
extern const TimezoneEntry TIMEZONES[];
extern const size_t TIMEZONE_COUNT;
// Magic-Header-Deklaration
extern const char OTA_MAGIC_HEADER[];
// ---------------------------------------------------------
// FieldConfig
// ---------------------------------------------------------

struct FieldConfig {
    String label;      // UI label
    String display;    // NEW: MQTT display name (CamelCase)
    String factor;
    String unit;
    bool mqtt;
    bool send;
};

// ---------------------------------------------------------
// Battery configuration
// ---------------------------------------------------------
struct BatteryConfig {
    unsigned long intervalPwr  = 60000;
    unsigned long intervalBat  = 300000;
    unsigned long intervalStat = 1800000;

    bool enableBat  = false;
    bool enableStat = false;

    bool useFahrenheit = false;

    uint8_t maxModules = 16;

    std::map<String, FieldConfig> fieldsPwr;
    std::map<String, FieldConfig> fieldsBat;
    std::map<String, FieldConfig> fieldsStat;

    float cellDiffWarn = 0.010f;   // 10 mV Warnschwelle
    float cellDiffError = 0.020f;  // 20 mV Fehlerschwelle

};

// ---------------------------------------------------------
// MQTT configuration
// ---------------------------------------------------------
struct MqttConfig {
    bool enabled = false;

    String server = "";
    uint16_t port = 1883;
    String user = "";
    String pass = "";

    String prefix     = "Pylontech";
    String topicStack = "Stack";
    String topicPwr   = "pwr";
    String topicBat   = "bat";
    String topicStat  = "stat";
    String cellPrefix = "Cell";   // NEW: configurable cell prefix

    String mode = "active";
};

// ---------------------------------------------------------
// Battery data structures (PWR / BAT / STAT)
// ---------------------------------------------------------
struct BatteryModule {
    bool present = false;
    int hub   = 0;   // 0 = no hub / stack mode
    int stack = 0;   // 1..5 in hub mode
    int index = 0;   // module index (Power/Module)
    int voltage_mV = 0;
    int current_mA = 0;
    int temperature = 0;
    int soc = 0;
    std::map<String, String> fields;
};

struct BatteryStack {
    int batteryCount = 0;
    int avgVoltage_mV = 0;
    int totalCurrent_mA = 0;
    int temperature = 0;
    int soc = 0;

    int stackID = 0;   // <--- ADD THIS
    int hubID = 0;     // <--- OPTIONAL, useful for Masterhub

    void reset() {
        batteryCount = 0;
        avgVoltage_mV = 0;
        totalCurrent_mA = 0;
        temperature = 0;
        soc = 0;
    }
};

struct BatField {
    String name;
    String raw;
    // int moduleIndex;  // optional
};

struct BatData {
    int moduleIndex;               // NEW: module number (1..N)
    int cellIndex = -1;
    std::vector<BatField> fields;
};

struct StatField {
    String name;
    String raw;
};

struct StatData {
    int moduleIndex = -1;
    std::vector<StatField> fields;
};

// ---------------------------------------------------------
// ParsedData + Double Buffer
// ---------------------------------------------------------
struct ParsedData {
    BatteryStack stack;
    std::vector<BatteryModule> modules;
    std::vector<BatData> batCells;
    StatData stat;
};

//extern ParsedData bufferA;      //alt
//extern ParsedData bufferB;      //alt
//extern volatile bool useA;      //alt

struct PwrBuffer {
    BatteryStack stack;
    std::vector<BatteryModule> modules;
};

struct BatBuffer {
    std::vector<BatData> cells;
};

struct StatBuffer {
    StatData stat;
};

// Doppelbuffer für PWR
extern PwrBuffer pwrA;
extern PwrBuffer pwrB;
extern volatile bool pwrUseA;

// Doppelbuffer für BAT
extern BatBuffer batA;
extern BatBuffer batB;
extern volatile bool batUseA;

// Doppelbuffer für STAT
extern StatBuffer statA;
extern StatBuffer statB;
extern volatile bool statUseA;


enum ParseResult {
    PARSE_OK,
    PARSE_FAIL,
    PARSE_IGNORED
};

#define MAX_MODULES 80

enum FrameType {
    FRAME_PWR,
    FRAME_BAT,
    FRAME_STAT
};

struct ParsedFrame {
    FrameType type;
    uint8_t index;

    BatteryStack stack;
    BatteryModule modules[MAX_MODULES];
    BatData bat;
    StatData stat;
};
// ---------------------------------------------------------
// Battery operating mode (Stack / Hub / Unknown)
// ---------------------------------------------------------
enum class BatteryMode {
    UNKNOWN = 0,
    STACK   = 1,
    HUB     = 2
};

// Global mode variable
extern BatteryMode g_batteryMode;


// ---------------------------------------------------------
// Parser / MQTT Flags
// ---------------------------------------------------------
extern bool parserHasData;
extern bool newParserData;

extern bool batParserHasData;
extern int  batParserModuleIndex;

extern bool statParserHasData;
extern int  statParserModuleIndex;

extern bool discoveryPwrNeeded;
extern bool discoveryBatNeeded;
extern bool discoveryStatNeeded;

// ---------------------------------------------------------
// AppConfig
// ---------------------------------------------------------
class AppConfig {
public:
    String deviceName = "PylontechMonitor";
    String hostname   = "";
    String wifiSSID   = "";
    String wifiPass   = "";
    String apSSID     = "";
    String apPass     = "";
    bool setupDone    = false;

    bool useStaticIP = false;
    String ipAddr     = "";
    String subnetMask = "";
    String gateway    = "";
    String dns        = "";

    String ntpServer = "pool.ntp.org";
    bool manual_mode = false;
    bool manual_dst = false;
    bool use_gateway_ntp = true;
    bool manual_ntp = false;
    String manual_date = "";
    String manual_time = "";

    String timezone = "Europe/Berlin";
    bool daylightSaving = true;
    uint32_t ntpResyncInterval = 86400;

    MqttConfig mqtt;
    BatteryConfig battery;

    String firmwareVersion = "1.2.1";
    String currentTime     = "";
    String lastPwrUpdate   = "";
    uint16_t detectedModules = 0;

    String lastMqttContact = "";

    void load();
    void save();

    void loadSystemConfig();
    void saveSystemConfig();

    void loadPwrConfig();
    void savePwrConfig();
    void loadPwrFields();
    void savePwrFields();

    void loadBatConfig();
    void saveBatConfig();
    void loadBatFields();
    void saveBatFields();

    void loadStatConfig();
    void saveStatConfig();
    void loadStatFields();
    void saveStatFields();

    void clearNVS();
    void factoryDefaults();
    void factoryReset();

    String uptimeString();
    String getCurrentTimeString();
    bool isSystemTimeValid();

    bool logInfo  = true;
    bool logWarn  = true;
    bool logError = true;
    bool logDebug = false;

private:
    String generateHostname();
    void saveJsonChunked(const char* ns, const char* prefix, const String& json);
    String loadJsonChunked(const char* ns, const char* prefix);
};
struct ModuleHealth {
    int index = 0;

    float tempMax = 0;      // höchste Temperatur (Tempr oder Thigh)
    float cellMin = 0;      // Vlow
    float cellMax = 0;      // Vhigh
    float cellDiff = 0;     // Delta

    String strongestState = "Normal";  // stärkster Status
    String status = "OK";              // OK / Warnung / Fehler
};

struct HealthStatus {
    std::vector<ModuleHealth> modules;

    std::vector<int> okModules;
    std::vector<int> warnModules;
    std::vector<int> errorModules;

    std::vector<int> warnHistory;
    std::vector<int> errorHistory;

    float stackCellMin = 0;
    float stackCellMax = 0;
    float stackCellDiff = 0;

    String strongestMessage = "OK";
    String color = "green"; // green, yellow, red
};

extern HealthStatus health;


extern AppConfig config;