/***********************************************************************
 Q150 Dew Controller
 Board : Seeed XIAO ESP32S3
 Control: BLE only
 Sensor : SHT45 (I2C)
 PWM    : D8 (Pin 9, Digital IO D8)
 I2C    : SDA=D4 (Pin 5), SCL=D5 (Pin 6)

 Firmware Version: 1.0
************************************************************************/

#define FIRMWARE_VERSION "1.1"
#define DEBUG 0   // <<<<<<<<<< SET TO 0 FOR RELEASE BUILD

// =====================================================================
// ========================== DEBUG MACROS ===============================
// =====================================================================
#if DEBUG
  #define DEBUG_BEGIN(x)   Serial.begin(x)
  #define DEBUG_PRINT(x)   Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
#else
  #define DEBUG_BEGIN(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Adafruit_SHT4x.h>

// =====================================================================
// =========================== HARDWARE =================================
// =====================================================================
#define HEATER_PWM_PIN D8           // Pin 9, Digital IO D8 on XIAO ESP32S3
#define I2C_SDA D4                   // Pin 5, Digital IO D4
#define I2C_SCL D5                   // Pin 6, Digital IO D5

static constexpr int PWM_FREQ_HZ   = 1000;
static constexpr int PWM_RES_BITS  = 10;
static constexpr int PWM_MAX       = (1 << PWM_RES_BITS) - 1;

// Timing intervals (milliseconds)
static constexpr unsigned long SENSOR_READ_INTERVAL_MS = 5000;   // 5 seconds
static constexpr unsigned long STATUS_NOTIFY_INTERVAL_MS = 2000; // 2 seconds

// =====================================================================
// =========================== SENSOR ===================================
// =====================================================================
static Adafruit_SHT4x sht4 = Adafruit_SHT4x();

// =====================================================================
// =========================== DEW MATH =================================
// =====================================================================
static float dewPointC(float T, float RH) {
  if (isnan(T) || isnan(RH) || RH <= 0.0f) return NAN;
  const float a = 17.62f;
  const float b = 243.12f;
  float g = (a * T) / (b + T) + logf(RH / 100.0f);
  return (b * g) / (a - g);
}

// =====================================================================
// ===================== SPREAD → POWER TABLE ============================
// =====================================================================
static constexpr int MAX_TABLE = 5;

struct SpreadPowerEntry {
  float   spreadC;
  uint8_t powerPct;
};

struct Config {
  uint8_t cfgVersion = 1;
  bool heaterEnabled = false;
  uint8_t count = 3;

  SpreadPowerEntry table[MAX_TABLE] = {
    {5.0f, 15},
    {3.0f, 30},
    {1.0f, 50},
    {0.0f, 0},
    {0.0f, 0}
  };
};

static Config g_cfg;
static Preferences g_prefs;

// Forward declarations
static void sortTableDescending(Config& cfg);
static uint8_t powerFromSpread(const Config& cfg, float spreadC);


// =====================================================================
// =========================== SENSOR ===================================
// =====================================================================

// =====================================================================
// =========================== STATE ====================================
// =====================================================================
static float g_T = NAN;
static float g_RH = NAN;
static float g_Td = NAN;
static float g_spread = NAN;
static uint8_t g_powerPct = 0;
static bool g_manual_mode = false;   // manual power override mode
static uint8_t g_manual_power = 0;   // manual power value (0-100)

// =====================================================================
// ============================= BLE ====================================
// =====================================================================
static const char* BLE_DEVICE_NAME = "Q150DewController";

#define SVC_UUID    "ab120000-0000-0000-0000-000000000001"
#define STATUS_UUID "ab120000-0000-0000-0000-000000000002"
#define CONFIG_UUID "ab120000-0000-0000-0000-00000000C003"
#define INFO_UUID   "ab120000-0000-0000-0000-000000000004"
#define CMD_UUID    "ab120000-0000-0000-0000-000000000005"

static BLEServer* g_server = nullptr;
static BLECharacteristic* g_statusChar = nullptr;
static BLECharacteristic* g_configChar = nullptr;
static BLECharacteristic* g_infoChar = nullptr;
static BLECharacteristic* g_cmdChar = nullptr;

// BLE pending operations (flags + buffers)
static constexpr size_t CONFIG_BUFFER_SIZE = 512;
static constexpr size_t CMD_BUFFER_SIZE = 64;

static volatile bool g_configWritePending = false;
static char g_configWriteBuffer[CONFIG_BUFFER_SIZE];
static volatile bool g_cmdWritePending = false;
static char g_cmdWriteBuffer[CMD_BUFFER_SIZE];

// =====================================================================
// ======================== BLE CALLBACKS ===============================
// =====================================================================
class ServerCallbacks : public BLEServerCallbacks {
public:
  void onConnect(BLEServer* pServer) {
    DEBUG_PRINTLN(F("✅ BLE CLIENT CONNECTED"));
  }
  
  void onDisconnect(BLEServer* pServer) {
    DEBUG_PRINTLN(F("❌ BLE disconnected — restarting advertising"));
  }
};

class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* pCharacteristic) {
    // Minimal - value is pre-set in loop()
  }
  
  void onWrite(BLECharacteristic* pCharacteristic) {
    DEBUG_PRINTLN(F("📝 CONFIG callback triggered"));
    // Minimal - just capture and flag (thread-safe with fixed buffer)
    String value = pCharacteristic->getValue();
    size_t len = min(value.length(), CONFIG_BUFFER_SIZE - 1);
    memcpy(g_configWriteBuffer, value.c_str(), len);
    g_configWriteBuffer[len] = '\0';
    g_configWritePending = true;
    DEBUG_PRINTLN(F("📝 CONFIG callback complete"));
  }
};

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    DEBUG_PRINTLN(F("🎮 CMD callback triggered"));
    // Minimal - just capture and flag (thread-safe with fixed buffer)
    String value = pCharacteristic->getValue();
    size_t len = min(value.length(), CMD_BUFFER_SIZE - 1);
    memcpy(g_cmdWriteBuffer, value.c_str(), len);
    g_cmdWriteBuffer[len] = '\0';
    g_cmdWritePending = true;
    DEBUG_PRINTLN(F("🎮 CMD callback complete"));
  }
};

// Create static callback instances
static ServerCallbacks serverCallbacks;
static ConfigCallbacks configCallbacks;
static CmdCallbacks cmdCallbacks;

// ======================
// =========================== INFO JSON ================================
// =====================================================================
static String encodeInfoJson() {
  StaticJsonDocument<192> doc;
  doc[F("device")]  = F("Q150DewController");
  doc[F("version")] = FIRMWARE_VERSION;
  doc[F("board")]   = F("XIAO ESP32S3");
  doc[F("pwm")]     = F("D8");
  doc[F("debug")]   = DEBUG ? F("on") : F("off");

  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================================
// ========================== STATUS JSON ===============================
// =====================================================================
static String encodeStatusJson() {
  StaticJsonDocument<256> doc;
  doc[F("T")]       = g_T;
  doc[F("RH")]      = g_RH;
  doc[F("Td")]      = g_Td;
  doc[F("spread")]  = g_spread;
  doc[F("power")]   = g_powerPct;
  doc[F("enabled")] = g_cfg.heaterEnabled;
  doc[F("source")]  = g_manual_mode ? "manual" : "sensor";

  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================================
// ========================== CONFIG JSON ===============================
// =====================================================================
static String encodeConfigJson() {
  StaticJsonDocument<512> doc;
  doc[F("cfgVersion")] = g_cfg.cfgVersion;
  doc[F("heaterEnabled")] = g_cfg.heaterEnabled;
  
  JsonArray arr = doc.createNestedArray(F("table"));
  for (int i = 0; i < g_cfg.count; i++) {
    JsonObject entry = arr.createNestedObject();
    entry[F("spread")] = g_cfg.table[i].spreadC;
    entry[F("power")] = g_cfg.table[i].powerPct;
  }
  
  String out;
  serializeJson(doc, out);
  return out;
}

static void decodeConfigJson(const String& json) {
  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, json);
  if (err) {
    DEBUG_PRINTLN(F("CONFIG JSON parse error"));
    return;
  }
  
  // Config version (optional, defaults to 1)
  if (doc.containsKey(F("cfgVersion"))) {
    g_cfg.cfgVersion = doc[F("cfgVersion")] | 1;
  }
  
  // Heater enabled
  if (doc.containsKey(F("heaterEnabled"))) {
    bool newState = doc[F("heaterEnabled")];
    if (newState != g_cfg.heaterEnabled) {
      DEBUG_PRINT(F("🔥 Heater state changed: "));
      DEBUG_PRINT(g_cfg.heaterEnabled ? F("ON") : F("OFF"));
      DEBUG_PRINT(F(" -> "));
      DEBUG_PRINTLN(newState ? F("ON") : F("OFF"));
    }
    g_cfg.heaterEnabled = newState;
  }
  
  // Table
  if (doc.containsKey(F("table"))) {
    JsonArray arr = doc[F("table")];
    g_cfg.count = min((int)arr.size(), MAX_TABLE);
    for (int i = 0; i < g_cfg.count; i++) {
      JsonObject entry = arr[i];
      g_cfg.table[i].spreadC = entry[F("spread")] | 0.0f;
      g_cfg.table[i].powerPct = entry[F("power")] | 0;
    }
    sortTableDescending(g_cfg);
  }

  saveConfig();
  DEBUG_PRINTLN(F("CONFIG updated"));
}

// =====================================================================
// =============== Connection Monitor ==================================
// =====================================================================
static void bleWatchdog() {
  static bool wasConnected = false;

  if (!g_server) return;

  bool connected = g_server->getConnectedCount() > 0;

  if (wasConnected && !connected) {
    DEBUG_PRINTLN(F("BLE disconnected (watchdog) — restart advertising"));
    BLEDevice::startAdvertising();
  }

  wasConnected = connected;
}

// =====================================================================
// =========================== HELPERS ==================================
// =====================================================================
static void pwmWritePercent(uint8_t pct) {
  pct = constrain(pct, 0, 100);
  int duty = (pct * PWM_MAX) / 100;
  ledcWrite(HEATER_PWM_PIN, duty);
  // DEBUG_PRINT(F("PWM: "));
  // DEBUG_PRINT(pct);
  // DEBUG_PRINT(F("% duty="));
  // DEBUG_PRINTLN(duty);
}

static void sortTableDescending(Config& cfg) {
  for (int i = 0; i < cfg.count; i++) {
    for (int j = i + 1; j < cfg.count; j++) {
      if (cfg.table[j].spreadC > cfg.table[i].spreadC) {
        auto tmp = cfg.table[i];
        cfg.table[i] = cfg.table[j];
        cfg.table[j] = tmp;
      }
    }
  }
}

static uint8_t powerFromSpread(const Config& cfg, float spreadC) {
  if (!cfg.heaterEnabled) return 0;
  if (isnan(spreadC)) return 30;

  for (int i = 0; i < cfg.count; i++) {
    if (spreadC < cfg.table[i].spreadC)
      return cfg.table[i].powerPct;
  }
  return 0;
}

// =====================================================================
// =================== BLE PENDING OPERATION HANDLERS ===================
// =====================================================================
static void processPendingConfigWrite() {
  if (!g_configWritePending) return;
  
  DEBUG_PRINTLN(F("Processing CONFIG write..."));
  DEBUG_PRINT(F("CONFIG received: "));
  DEBUG_PRINTLN(g_configWriteBuffer);
  
  decodeConfigJson(g_configWriteBuffer);
  
  DEBUG_PRINTLN(F("CONFIG written"));
  g_configWritePending = false;
}

static void processPendingCmdWrite() {
  if (!g_cmdWritePending) return;
  
  String cmd = g_cmdWriteBuffer;
  DEBUG_PRINT(F("CMD received: "));
  DEBUG_PRINTLN(cmd);
  
  // Parse command: "power:XX"
  if (cmd.startsWith("power:")) {
    int power = cmd.substring(6).toInt();
    if (power >= 0 && power <= 100) {
      g_manual_mode = true;
      g_manual_power = power;
      g_powerPct = power;
      pwmWritePercent(power);
      DEBUG_PRINTLN("Manual power: " + String(power) + "%");
    }
  }
  // "auto" command to return to automatic mode
  else if (cmd == "auto") {
    g_manual_mode = false;
    DEBUG_PRINTLN(F("Returning to automatic mode"));
  }
  
  g_cmdWritePending = false;
}

// =====================================================================
// ======================= SENSOR READ ==================================
// =====================================================================
static bool readSensor(float* outT, float* outRH) {
  sensors_event_t humidity, temp;
  if (sht4.getEvent(&humidity, &temp)) {
    *outT = temp.temperature;
    *outRH = humidity.relative_humidity;
    if (!isnan(*outT) && !isnan(*outRH)) {
      DEBUG_PRINTLN(F("✅ SHT45 sensor data"));
      return true;
    }
  }
  DEBUG_PRINTLN(F("❌ SHT45 sensor read failed"));
  return false;
}

// =====================================================================
// ===================== CONFIG PERSISTENCE =============================
// =====================================================================
static void saveConfig() {
  g_prefs.begin("q150dew", false);
  g_prefs.putUChar("cfgVer", g_cfg.cfgVersion);
  g_prefs.putBool("heaterEn", g_cfg.heaterEnabled);
  g_prefs.putUChar("count", g_cfg.count);

  for (int i = 0; i < MAX_TABLE; i++) {
    g_prefs.putFloat(("sp" + String(i)).c_str(), g_cfg.table[i].spreadC);
    g_prefs.putUChar(("pw" + String(i)).c_str(), g_cfg.table[i].powerPct);
  }

  g_prefs.end();

  DEBUG_PRINTLN(F("Config saved"));
}

static void loadConfig() {
  g_prefs.begin("q150dew", true);

  g_cfg.cfgVersion = g_prefs.getUChar("cfgVer", 1);
  g_cfg.heaterEnabled = g_prefs.getBool("heaterEn", false);
  g_cfg.count = constrain(g_prefs.getUChar("count", g_cfg.count), 1, MAX_TABLE);

  for (int i = 0; i < MAX_TABLE; i++) {
    g_cfg.table[i].spreadC =
      g_prefs.getFloat(("sp" + String(i)).c_str(), g_cfg.table[i].spreadC);
    g_cfg.table[i].powerPct =
      g_prefs.getUChar(("pw" + String(i)).c_str(), g_cfg.table[i].powerPct);
  }

  g_prefs.end();

  sortTableDescending(g_cfg);

  DEBUG_PRINTLN(F("Config loaded"));
}

// =====================================================================
// =========================== SETUP ====================================
// =====================================================================
static void setupPWM() {
  ledcAttach(HEATER_PWM_PIN, PWM_FREQ_HZ, PWM_RES_BITS);
  pwmWritePercent(0);
}

static void setupSensor() {
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!sht4.begin()) {
    DEBUG_PRINTLN(F("SHT45 not found!"));
    return;
  }
  
  sht4.setPrecision(SHT4X_HIGH_PRECISION);
  sht4.setHeater(SHT4X_NO_HEATER);
  DEBUG_PRINTLN(F("SHT45 ready"));
}

static void setupBLE() {

  DEBUG_PRINTLN(F("BLE start"));

  BLEDevice::init(BLE_DEVICE_NAME);

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(&serverCallbacks);

  BLEService* svc = g_server->createService(SVC_UUID);

  g_statusChar = svc->createCharacteristic(
    STATUS_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY);
  g_statusChar->addDescriptor(new BLE2902());

  g_configChar = svc->createCharacteristic(
    CONFIG_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE);
  g_configChar->setCallbacks(&configCallbacks);

  String initialConfig = encodeConfigJson();
  g_configChar->setValue(initialConfig.c_str());
  DEBUG_PRINT(F("CONFIG initial value set: "));
  DEBUG_PRINTLN(initialConfig);

  g_infoChar = svc->createCharacteristic(
    INFO_UUID,
    BLECharacteristic::PROPERTY_READ);

  String initialInfo = encodeInfoJson();
  g_infoChar->setValue(initialInfo.c_str());
  DEBUG_PRINT(F("INFO initial value set: "));
  DEBUG_PRINTLN(initialInfo);

  g_cmdChar = svc->createCharacteristic(
    CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR);
  g_cmdChar->setCallbacks(&cmdCallbacks);
  DEBUG_PRINTLN(F("CMD characteristic created with callbacks"));

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();

  BLEAdvertisementData advData;
  advData.setName(BLE_DEVICE_NAME);
  advData.setCompleteServices(BLEUUID(SVC_UUID));

  adv->setAdvertisementData(advData);
  adv->setScanResponse(false);

  BLEDevice::startAdvertising();

  DEBUG_PRINTLN(F("BLE ready"));
}

// =====================================================================
// ============================== LOOP ==================================
// =====================================================================
static void updateReadingsAndControl() {
  float T, H;

  if (!readSensor(&T, &H)) {
    DEBUG_PRINTLN(F("Sensor read failed"));
    return;
  }

  g_T = T;
  g_RH = H;
  g_Td = dewPointC(g_T, g_RH);
  g_spread = isnan(g_Td) ? NAN : (g_T - g_Td);

  // Use manual power if in manual mode, otherwise calculate from spread
  if (g_manual_mode) {
    g_powerPct = g_manual_power;
  } else {
    g_powerPct = powerFromSpread(g_cfg, g_spread);
  }
  pwmWritePercent(g_powerPct);

  // DEBUG_PRINT(F("Sp="));  // Commented out to reduce log spam
  // DEBUG_PRINT(g_spread);
  // DEBUG_PRINT(F(" P="));
  // DEBUG_PRINTLN(g_powerPct);
}

void setup() {
  DEBUG_BEGIN(115200);
  
  #if DEBUG
    // Wait for Serial Monitor to connect (up to 5 seconds)
    unsigned long start = millis();
    while (!Serial && (millis() - start < 5000)) {
      delay(10);
    }
    delay(800);  // Extra settle time
  #endif

  DEBUG_PRINTLN();
  DEBUG_PRINTLN(F("========================================"));
  DEBUG_PRINTLN(F("Q150 Dew Controller"));
  DEBUG_PRINT(F("FW "));
  DEBUG_PRINTLN(FIRMWARE_VERSION);
  DEBUG_PRINTLN(F("========================================"));

  loadConfig();
  setupPWM();
  setupSensor();
  setupBLE();
}

void loop() {
  static unsigned long lastRead = 0;
  static unsigned long lastNotify = 0;
  static unsigned long lastConfigUpdate = 0;
  unsigned long now = millis();

  // Process pending BLE operations FIRST (minimal latency)
  processPendingConfigWrite();
  processPendingCmdWrite();

  // check BLE connection
  bleWatchdog();

  if (now - lastRead >= SENSOR_READ_INTERVAL_MS) {
    lastRead = now;
    updateReadingsAndControl();
  }

  if (now - lastNotify >= STATUS_NOTIFY_INTERVAL_MS && g_statusChar) {
    lastNotify = now;
    String status = encodeStatusJson();
    g_statusChar->setValue(status.c_str());
    g_statusChar->notify();
  }

  delay(10);
}
