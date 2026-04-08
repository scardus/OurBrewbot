#pragma once
/*
 * Config.h — All data structures and configuration constants
 *
 * JSON field names match the original firmware exactly so existing LittleFS
 * config files will load without migration.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// ============================================================
// SYSTEM LIMITS
// ============================================================
#define MAX_FERMENTERS      4
#define MAX_PROBES          8
#define MAX_SMART_PLUGS     10
#define MAX_PROFILE_STEPS   60
#define MAX_PROFILES        4
#define MAX_STEPS_PER_PROFILE 15  // Steps per profile (4 profiles × 15 = 60)
#define MAX_ISPINDELS       4
#define MAX_TILTS           8       // 8 Tilt colours supported (indexed by colour)
#define MAX_TILT_SLOTS      4       // Configurable Tilt slots persisted in jsonTilt.txt
#define PROBE_UNASSIGNED    99      // Sentinel value from original firmware
#define PROBE_FAIL_THRESHOLD 6      // Consecutive failed reads before marking inactive (~30s at 5s poll)

// ============================================================
// TEMPERATURE UNITS
// ============================================================
#define UNIT_CELSIUS        1
#define UNIT_FAHRENHEIT     2

// ============================================================
// BREW SERVICES
// ============================================================
#define BREW_SERVICE_NONE           0
#define BREW_SERVICE_BREWERS_FRIEND 1
#define BREW_SERVICE_BREWFATHER     2
#define BREW_SERVICE_MQTT           3
#define MAX_BREW_SERVICES           2   // HTTP services: 0=Brewer's Friend, 1=Brewfather
#define MQTT_SERVICE_BIT            3   // Bit index in fermenter brewServices bitmask (matches BREW_SERVICE_MQTT)

// ============================================================
// PROBE FUNCTION CODES
// ============================================================
#define PROBE_FN_UNASSIGNED  99
#define PROBE_FN_BEER        2
#define PROBE_FN_AMBIENT     3
#define PROBE_FN_TILT        4    // "Tiltprobe"
#define PROBE_FN_ISPINDEL    5    // "Ispindelprobe"
#define PROBE_FN_AIR         6    // "Airprobe"
#define PROBE_FN_CONTROL     7    // "Controltemp"

// ============================================================
// SMART PLUG FUNCTION CODES
// ============================================================
#define PLUG_FN_UNASSIGNED   99
#define PLUG_FN_F1_HOT       0
#define PLUG_FN_F1_COLD      1
#define PLUG_FN_F2_HOT       2
#define PLUG_FN_F2_COLD      3
#define PLUG_FN_F3_HOT       4
#define PLUG_FN_F3_COLD      5
#define PLUG_FN_F4_HOT       6
#define PLUG_FN_F4_COLD      7
#define PLUG_FN_AUX1         8
#define PLUG_FN_AUX2         9

// ============================================================
// PROFILE STEP TYPES
// ============================================================
enum ProfileStepType {
  STEP_TEMP_OVER_TIME    = 0,  // "Changes step after x days and temp at target."
  STEP_TIME_OVER_TEMP    = 1,  // "Changes step after x days have run."
  STEP_FREE_RISE         = 2,  // "Changes step after x days."
  STEP_SPECIFIC_GRAVITY  = 3,  // "Changes step when SG at target."
  STEP_ATTENUATION       = 4,  // "Changes step when Attn% at target."
  STEP_TEMP_REACHED      = 5,  // "Changes step when temp reached."
  STEP_SG_REACHED        = 6,  // "Changes step when SG reached."
  STEP_ATTN_REACHED      = 7,  // "Changes step when Attn% reached."
  STEP_TIME_AND_SG       = 8,  // "Changes step after x days and SG at target."
  STEP_TIME_AND_ATTN     = 9,  // "Changes step after x days and Attn% at target."
};

// ============================================================
// TILT HYDROMETER COLOURS
// ============================================================
enum TiltColour {
  TILT_RED    = 0,
  TILT_GREEN  = 1,
  TILT_BLACK  = 2,
  TILT_PURPLE = 3,
  TILT_ORANGE = 4,
  TILT_BLUE   = 5,
  TILT_YELLOW = 6,
  TILT_PINK   = 7,
};

// ============================================================
// REBOOT REASONS
// ============================================================
enum RebootReason {
  REBOOT_POWER_ON        = 0,  // "Power on"
  REBOOT_HW_WATCHDOG     = 1,  // "Hardware Watchdog"
  REBOOT_EXCEPTION       = 2,  // "Exception"
  REBOOT_SW_WATCHDOG     = 3,  // "Software Watchdog"
  REBOOT_SW_RESTART      = 4,  // "Software/System restart"
  REBOOT_DOUBLE_RESET    = 5,  // "Double Reset"
  REBOOT_OTA             = 6,  // "OTA_UPGRADE"
  REBOOT_RESET_CONFIG    = 7,  // "Resetting configuration!"
};

// ============================================================
// CONTROLLER HARDWARE VERSIONS
// ============================================================
enum ControllerVersion {
  CTRL_VERSION_1   = 1,
  CTRL_VERSION_2   = 2,
  CTRL_VERSION_3   = 3,
  CTRL_VERSION_4   = 4,
  CTRL_NEXT_GEN    = 5,  // "Controller = Next Gen"
};

// ============================================================
// LittleFS FILENAMES
// All files have a backup copy
// ============================================================
#define FILE_CONFIG         "/jsonConfig.txt"
#define FILE_CONFIG_BKP     "/jsonConfigbkup.txt"
#define FILE_GLOBAL         "/jsonGlobal.txt"
#define FILE_GLOBAL_BKP     "/jsonGlobalbkup.txt"
#define FILE_FERMENTER      "/jsonFermenter.txt"
#define FILE_FERMENTER_BKP  "/jsonFermenterbkup.txt"
#define FILE_PROBE          "/jsonProbe.txt"
#define FILE_PROBE_BKP      "/jsonProbebkup.txt"
#define FILE_SMARTPLUGS     "/jsonSmartPlugs.txt"
#define FILE_SMARTPLUGS_BKP "/jsonSmartPlugsbkup.txt"
#define FILE_PROFILE        "/jsonProfile.txt"
#define FILE_PROFILE_BKP    "/jsonProfilebkup.txt"
#define FILE_STEPS          "/jsonProfileSteps.txt"
#define FILE_STEPS_BKP      "/jsonProfileStepsbkup.txt"
#define FILE_TILT           "/jsonTilt.txt"
#define FILE_TILT_BKP       "/jsonTiltbkup.txt"
#define FILE_ISPINDEL       "/jsoniSpindel.txt"
#define FILE_ISPINDEL_BKP   "/jsoniSpindelbkup.txt"
#define FILE_PLAATO         "/jsonPlaato.txt"
#define FILE_PLAATO_BKP     "/jsonPlaatobkup.txt"
#define FILE_BREWSVC        "/jsonBrewServices.txt"
#define FILE_BREWSVC_BKP    "/jsonBrewServicesbkup.txt"
#define FILE_MQTT           "/jsonMqtt.txt"
#define FILE_MQTT_BKP       "/jsonMqttbkup.txt"
#define FILE_SYSLOG         "/jsonSyslog.txt"
#define FILE_SYSLOG_BKP     "/jsonSyslogbkup.txt"
#define FILE_REBOOT         "/jsonReBoot.txt"
#define FILE_REBOOT_BKP     "/jsonReBootbkup.txt"
#define FILE_CHART_SERIES   "/jsonchartSeries.txt"
#define FILE_CHART_NAMES    "/jsonchartNames.txt"
#define FILE_DRD            "/DRD.txt"
#define FILE_NG_VERSION     "/NG_version.txt"

// ============================================================
// STRUCT: GlobalConfig
// Persisted in jsonGlobal.txt
// ============================================================
struct GlobalConfig {
  char    authCode[48];
  uint8_t unit;                 // 1=Celsius, 2=Fahrenheit
  bool    notifyOn;             // push notifications enabled
  char    brewServiceId[32];    // brew service user ID
  uint8_t brewService;          // 0-3, see BREW_SERVICE_xxx
  bool    migrated;             // config migration flag
  uint16_t bleBaud;             // BLE baud rate
  uint32_t lastUptime;          // minutes uptime counter
  uint8_t  swNo;                // software number (e.g. 20)
  uint8_t  sendToCloud;         // cloud send enable
  uint8_t  globalSave;          // global save flag
  char     myBrewBuddy[32];     // MyBrewBuddy BLE address
  char     bSitterAuth[48];     // babysitter auth token
  uint8_t  babySitter;          // babysitter mode
  bool     plugCategory;        // plug category flag
  uint8_t  fNo;                 // fermenter number
  uint8_t  mbbHardReset;        // MyBrewBuddy hard reset flag
  uint8_t  tuningChartNo;       // tuning chart number
  uint8_t  resolution;          // DS18B20 resolution 9-12 bits (e.g. 11)
};

// ============================================================
// STRUCT: FermenterConfig
// Persisted in jsonFermenter.txt
// ============================================================
struct FermenterConfig {
  char    fermenterName[32];    // e.g. "Fermenter 1"
  char    beerName[32];
  char    yeastName[32];
  char    bjcp[16];             // BJCP category code
  float   ceilingTemp;          // max temp °C
  float   floorTemp;            // min temp °C
  float   og;                   // original gravity (e.g. 1047)
  float   tg;                   // target gravity (e.g. 1010)
  float   hysteresis;           // temp swing allowance (e.g. 0.1)
  uint16_t compressorDelay;     // minutes between cooling cycles (e.g. 10)
  bool    tempControl;          // temperature control active
  bool    sgControl;            // gravity control active
  bool    power;                // fermenter powered
  float   alarmTolerance;       // alarm threshold
  float   ambientSG;            // ambient specific gravity
  bool    alarm;                // alarm state
  uint8_t profileNo;            // assigned profile: 0=standard, 1-4=profile
  uint8_t currentStep;          // current step within profile (0-14)
  uint16_t currentHour;         // hours elapsed in current profile step
  bool    liveTest;             // live test mode (fast-forward 1hr per 10s)
  uint8_t status;               // current status: 0=idle,1=heating,2=cooling
  bool    profileRunning;       // profile is active
  uint8_t brewServices;          // bitmask: bit N = send to g_brewServices[N]
  bool    psiCollect;           // collect pressure data
  uint8_t function;             // fermenter function code
  uint8_t series1;              // chart series 1 type
  uint8_t series2;              // chart series 2 type
  uint8_t series3;              // chart series 3 type
  uint8_t series4;              // chart series 4 type
  float   sgCalibration;        // SG calibration offset
  bool    mbbPsiCollect;        // MyBrewBuddy PSI collect
  uint32_t startMillis;         // profile start time in millis
};

// ============================================================
// STRUCT: ProbeConfig
// Persisted in jsonProbe.txt
// ============================================================
struct ProbeConfig {
  char    probeName[24];        // e.g. "Beer Temp" / "Fridge Temp"
  char    address[20];          // OneWire address hex string (8 bytes = 16 hex chars + null)
  uint8_t function;             // see PROBE_FN_xxx
  uint8_t fermenter;            // assigned fermenter index
  float   temperature;          // current reading °C
  uint8_t mbb;                  // from MyBrewBuddy
  float   rawTemperature;       // before adjustment
  float   tempAdjust;           // calibration offset
  float   sgAdjust;             // SG calibration offset
  uint8_t failCount;            // consecutive failed reads (runtime only, not persisted)
};

// ============================================================
// STRUCT: SmartPlugConfig
// Persisted in jsonSmartPlugs.txt
// ============================================================
struct SmartPlugConfig {
  uint8_t  type;                // plug type
  uint8_t  codeset;             // RF codeset
  uint8_t  protocol;            // RF protocol
  uint8_t  bits;                // RF bit count (e.g. 24)
  uint16_t delayLength;         // RF pulse width µs (e.g. 427)
  uint8_t  function;            // plug function PLUG_FN_xxx
  uint8_t  fermenter;           // assigned fermenter
  uint32_t onCode;              // RF on code
  uint32_t offCode;             // RF off code
  char     manufacturer[24];    // e.g. "Dial" / "Brennenstuhl" / "Arlec"
  char     model[24];           // model name
  uint8_t  plugNo;              // plug slot number
};

// ============================================================
// STRUCT: ProfileConfig + ProfileStep
// Persisted in jsonProfile.txt and jsonProfileSteps.txt
// ============================================================
struct ProfileStep {
  uint8_t stepNo;               // step index
  uint8_t stepType;             // see ProfileStepType enum
  float   startTemp;            // starting temperature °C
  float   endTemp;              // target/ending temperature °C
  float   sgTrigger;            // SG trigger value
  float   days;                 // days for time-based steps (supports fractional)
  float   endSG;                // end gravity (alias for sgTrigger in some steps)
};

struct ProfileConfig {
  char profileName[32];         // "Empty Profile" (default)
};

// ============================================================
// STRUCT: TiltConfig
// Persisted in jsonTilt.txt — supports up to MAX_TILT_SLOTS (4) simultaneously
// configured Tilts. Array is indexed by colour (0-7), config loaded by Address.
// ============================================================
struct TiltConfig {
  // Persisted fields (jsonTilt.txt keys: Address, Function, Fermenter, Temp_Adjust, SG_Adjust, MBB)
  uint8_t colour;               // TiltColour enum (99 = unconfigured slot)
  uint8_t function;             // probe-like function code (PROBE_FN_xxx or 99=unassigned)
  uint8_t fermenter;            // assigned fermenter (0-3 or 99=unassigned)
  float   tempAdjust;           // temperature calibration offset °C
  float   sgAdjust;             // SG calibration offset
  uint8_t mbb;                  // MyBrewBuddy placeholder (unused)
  // Runtime data (not persisted)
  float   sg;                   // current specific gravity reading (with sgAdjust applied)
  float   temperature;          // Tilt temperature reading (with tempAdjust applied)
  bool    active;               // Tilt has been seen recently
  uint32_t lastSeen;            // millis() of last reading
};

// ============================================================
// STRUCT: iSpindelConfig
// Persisted in jsoniSpindel.txt
// ============================================================
struct iSpindelConfig {
  char     name[24];            // device name (e.g. "None" when unregistered)
  uint32_t id;                  // device ID
  bool     collectData;         // collect data from this device
  uint8_t  fermenter;           // assigned fermenter
  uint8_t  unit;                // unit: 0=SG, 1=Plato
  // Runtime data (not persisted)
  float    sg;                  // current SG reading
  float    temperature;         // current temp reading
  float    battery;             // battery voltage
  int8_t   rssi;                // WiFi signal strength
};

// ============================================================
// STRUCT: PlaatoConfig
// Persisted in jsonPlaato.txt
// ============================================================
struct PlaatoConfig {
  char    authCode[48];         // Plaato auth code
  bool    getData;              // fetch data from Plaato
};

// ============================================================
// STRUCT: WiFiConfig
// WiFi and cloud configuration — stored in non-volatile flash region
// ============================================================
struct WiFiConfig {
  uint32_t magic;               // config validity marker (0x627B4DAB)
  char     version[8];          // "1.0.1"
  bool     flagConfig;          // config has been set
  bool     flagApFail;          // AP mode failed flag
  bool     flagSelfTest;        // self-test mode
  char     wifiSSID[64];        // WiFi network name
  char     wifiPass[64];        // WiFi password
  char     cloudToken[48];      // legacy cloud auth token
  char     cloudHost[64];       // legacy cloud host
  uint16_t cloudPort;           // legacy cloud port
  uint32_t checksum;            // config checksum
};

// ============================================================
// STRUCT: BrewServiceConfig
// 2 fixed service types indexed by (BREW_SERVICE_xxx - 1):
//   [0] = Brewer's Friend, [1] = Brewfather
// ============================================================
struct BrewServiceConfig {
  bool    enabled;              // service active
  char    serviceId[48];        // API key / stream ID
  char    deviceName[32];       // device name (e.g. Brewfather "name" field)
};

// ============================================================
// STRUCT: SyslogConfig
// ============================================================
struct SyslogConfig {
  bool     enabled;        // syslog on/off
  char     host[64];       // syslog server hostname or IP
  uint16_t port;           // UDP port (default 514)
  uint8_t  facility;       // syslog facility (0–23, default 16 = local0)
  uint8_t  minLevel;       // minimum severity to send (0=EMERG … 7=DEBUG)
};

// ============================================================
// STRUCT: MqttConfig
// ============================================================
struct MqttConfig {
  bool    enabled;
  bool    haDiscovery;            // publish Home Assistant MQTT discovery configs
  char    host[64];
  uint16_t port;
  char    username[32];
  char    password[48];
  char    baseTopic[32];          // e.g. "ourbrewbot"
};

// ============================================================
// GLOBAL INSTANCES (defined in Config.cpp)
// ============================================================
extern GlobalConfig    g_globalConfig;
extern FermenterConfig g_fermenters[MAX_FERMENTERS];
extern ProbeConfig     g_probes[MAX_PROBES];
extern SmartPlugConfig g_smartPlugs[MAX_SMART_PLUGS];
extern ProfileConfig   g_profiles[MAX_PROFILES];
extern ProfileStep     g_profileSteps[MAX_PROFILE_STEPS];
extern TiltConfig      g_tilts[MAX_TILTS];
extern iSpindelConfig  g_iSpindels[MAX_ISPINDELS];
extern PlaatoConfig    g_plaato[MAX_ISPINDELS];
extern WiFiConfig      g_wifiConfig;
extern BrewServiceConfig g_brewServices[MAX_BREW_SERVICES];
extern MqttConfig        g_mqttConfig;
extern SyslogConfig      g_syslogConfig;

// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

// Load/save all config
void loadAllConfig();
void saveAllConfig();

// Individual file operations
bool loadGlobalConfig();
bool saveGlobalConfig();
bool loadFermenterConfig();
bool saveFermenterConfig();
bool loadProbeConfig();
bool saveProbeConfig();
bool loadSmartPlugConfig();
bool saveSmartPlugConfig();
bool loadProfileConfig();
bool saveProfileConfig();
bool loadProfileSteps();
bool saveProfileSteps();
bool loadTiltConfig();
bool saveTiltConfig();
bool loadiSpindelConfig();
bool saveiSpindelConfig();
bool loadPlaatoConfig();
bool savePlaatoConfig();
bool loadBrewServiceConfig();
bool saveBrewServiceConfig();
bool loadMqttConfig();
bool saveMqttConfig();
bool loadSyslogConfig();
bool saveSyslogConfig();

// Defaults / factory reset
void initDefaultGlobalConfig();
void initDefaultFermenterConfig();
void initDefaultProbeConfig();
void initDefaultSmartPlugConfig();
void initDefaultProfileConfig();
void initDefaultTiltConfig();
void initDefaultiSpindelConfig();
void initDefaultPlaatoConfig();
void initDefaultBrewServiceConfig();
void initDefaultMqttConfig();
void initDefaultSyslogConfig();
void resetWiFiConfig();
void resetAllConfig();

// Utility
String loadJsonFile(const char* path);
bool   saveJsonFile(const char* path, const String& json);
bool   saveJsonFileSafe(const char* primary, const char* backup, const String& json);

// Reboot logging
void recordReboot(const String& reason);
