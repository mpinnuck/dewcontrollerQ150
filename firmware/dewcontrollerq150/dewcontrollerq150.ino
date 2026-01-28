/***********************************************************************
 Q150 Dew Controller
 Board : Seeed XIAO ESP32-C3
 Control: BLE only
 Sensor : SHT45 (compiled in but DISABLED for now)
 Source : Weather Station ISYDNEY478
 PWM    : GPIO9 (Arduino pin D9 on XIAO ESP32-C3)

 Firmware Version: 0.9
************************************************************************/

#define FIRMWARE_VERSION "0.9"
#define DEBUG 1   // <<<<<<<<<< SET TO 0 FOR RELEASE BUILD

// =====================================================================
// ========================== DEBUG MACROS ===============================
// =====================================================================
#if DEBUG
  #define DEBUG_BEGIN(x)   Serial.begin(x)
  #define DEBUG_PRINT(x)   do { Serial.print("["); Serial.print(millis()); Serial.print("] "); Serial.print(x); } while(0)
  #define DEBUG_PRINTLN(x) do { Serial.print("["); Serial.print(millis()); Serial.print("] "); Serial.println(x); } while(0)
#else
  #define DEBUG_BEGIN(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

// =====================================================================
// =========================== HARDWARE =================================
// =====================================================================
#define HEATER_PWM_PIN D9           // GPIO9 on XIAO ESP32-C3

static constexpr int PWM_FREQ_HZ   = 1000;
static constexpr int PWM_RES_BITS  = 10;
static constexpr int PWM_MAX       = (1 << PWM_RES_BITS) - 1;

// =====================================================================
// ======================= WEATHER STATION ===============================
// =====================================================================
static const char* WEATHER_API_URL =
  "https://api.weather.com/v2/pws/observations/current?"
  "stationId=ISYDNEY478&format=json&units=m&apiKey=5356e369de454c6f96e369de450c6f22";

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
  bool heaterEnabled = true;
  uint8_t count = 3;

  SpreadPowerEntry table[MAX_TABLE] = {
    {5.0f, 15},
    {3.0f, 30},
    {1.0f, 50},
    {0.0f, 0},
    {0.0f, 0}
  };

  String wifiSSID = "MicroConcepts-2G";
  String wifiPass = "leanneannatinka";
};

static Config g_cfg;
static Preferences g_prefs;

// Forward declarations
static void sortTableDescending(Config& cfg);
static uint8_t powerFromSpread(const Config& cfg, float spreadC);


// =====================================================================
// =========================== SENSOR ===================================
// =====================================================================
enum class DataSource : uint8_t { WEATHER };
static DataSource g_source = DataSource::WEATHER;

// =====================================================================
// =========================== STATE ====================================
// =====================================================================
static float g_T = NAN;
static float g_RH = NAN;
static float g_Td = NAN;
static float g_spread = NAN;
static uint8_t g_powerPct = 0;

// =====================================================================
// ============================= BLE ====================================
// =====================================================================
static const char* BLE_DEVICE_NAME = "Q150DewController";

static NimBLEUUID SVC_UUID   ("ab120000-0000-0000-0000-000000000001");
static NimBLEUUID STATUS_UUID("ab120000-0000-0000-0000-000000000002");
static NimBLEUUID CONFIG_UUID("ab120000-0000-0000-0000-000000000003");
static NimBLEUUID INFO_UUID  ("ab120000-0000-0000-0000-000000000004");
static NimBLEUUID CMD_UUID   ("ab120000-0000-0000-0000-000000000005");

static NimBLECharacteristic* g_statusChar = nullptr;

// =====================================================================
// ======================== BLE CALLBACKS ===============================
// =====================================================================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    DEBUG_PRINTLN(F("========== BLE CLIENT CONNECTED =========="));
    DEBUG_PRINT(F("Connected clients: "));
    DEBUG_PRINTLN(pServer->getConnectedCount());
  }
  
  void onDisconnect(NimBLEServer* pServer) {
    DEBUG_PRINTLN(F("========== BLE CLIENT DISCONNECTED =========="));
    DEBUG_PRINT(F("Remaining clients: "));
    DEBUG_PRINTLN(pServer->getConnectedCount());
    
    DEBUG_PRINTLN(F("Attempting to restart advertising..."));
    
    // Try to restart advertising using the server method
    if (pServer->startAdvertising()) {
      DEBUG_PRINTLN(F("✓ Advertising restarted via pServer->startAdvertising()"));
    } else {
      DEBUG_PRINTLN(F("✗ pServer->startAdvertising() failed, trying NimBLEDevice method"));
      NimBLEDevice::startAdvertising();
      DEBUG_PRINTLN(F("✓ Advertising restarted via NimBLEDevice::startAdvertising()"));
    }
    
    DEBUG_PRINTLN(F("========== READY FOR NEW CONNECTION =========="));
  }
};

class ConfigCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* pCharacteristic) {
    DEBUG_PRINTLN(F("CONFIG onRead callback triggered"));
    String json = encodeConfigJson();
    DEBUG_PRINT(F("CONFIG JSON: "));
    DEBUG_PRINTLN(json);
    pCharacteristic->setValue(json);
    DEBUG_PRINTLN(F("CONFIG read complete"));
  }
  
  void onWrite(NimBLECharacteristic* pCharacteristic) {
    DEBUG_PRINTLN(F("CONFIG onWrite callback triggered"));
    std::string value = pCharacteristic->getValue();
    DEBUG_PRINT(F("CONFIG received: "));
    DEBUG_PRINTLN(value.c_str());
    decodeConfigJson(String(value.c_str()));
    DEBUG_PRINTLN(F("CONFIG written"));
  }
};

// ======================
// =========================== INFO JSON ================================
// =====================================================================
static String encodeInfoJson() {
  StaticJsonDocument<192> doc;
  doc[F("device")]  = F("Q150DewController");
  doc[F("version")] = FIRMWARE_VERSION;
  doc[F("board")]   = F("XIAO ESP32-C3");
  doc[F("pwm")]     = F("GPIO9");
  doc[F("source")]  = F("weather");
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
  doc[F("source")]  = F("weather");

  String out;
  serializeJson(doc, out);
  return out;
}

// =====================================================================
// ========================== CONFIG JSON ===============================
// =====================================================================
static String encodeConfigJson() {
  StaticJsonDocument<512> doc;
  doc[F("heaterEnabled")] = g_cfg.heaterEnabled;
  doc[F("count")] = g_cfg.count;
  
  JsonArray arr = doc.createNestedArray(F("table"));
  for (int i = 0; i < g_cfg.count; i++) {
    JsonObject entry = arr.createNestedObject();
    entry[F("spread")] = g_cfg.table[i].spreadC;
    entry[F("power")] = g_cfg.table[i].powerPct;
  }
  
  JsonObject wifi = doc.createNestedObject(F("wifi"));
  wifi[F("ssid")] = g_cfg.wifiSSID;
  wifi[F("configured")] = !g_cfg.wifiPass.isEmpty();
  // Never send password back
  
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
  
  // Heater enabled
  if (doc.containsKey(F("heaterEnabled"))) {
    g_cfg.heaterEnabled = doc[F("heaterEnabled")];
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
  
  // Wi-Fi
  if (doc.containsKey(F("wifi"))) {
    JsonObject wifi = doc[F("wifi")];
    if (wifi.containsKey(F("ssid"))) {
      g_cfg.wifiSSID = wifi[F("ssid")].as<String>();
    }
    if (wifi.containsKey(F("pass"))) {
      g_cfg.wifiPass = wifi[F("pass")].as<String>();
    }
  }
  
  saveConfig();
  DEBUG_PRINTLN(F("CONFIG updated"));
}

// =====================================================================
// =========================== HELPERS ==================================
// =====================================================================
static void pwmWritePercent(uint8_t pct) {
  pct = constrain(pct, 0, 100);
  int duty = (pct * PWM_MAX) / 100;
  ledcWrite(HEATER_PWM_PIN, duty);
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
// ======================= WEATHER FETCH ================================
// =====================================================================
static bool fetchOutdoorWeather(float* outT, float* outRH) {
  HTTPClient http;
  http.begin(WEATHER_API_URL);

  int code = http.GET();
  if (code != 200) {
    // DEBUG_PRINTLN(F("Weather fetch failed"));  // Commented out to reduce log spam
    http.end();
    return false;
  }

  StaticJsonDocument<1024> doc;
  auto err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    // DEBUG_PRINTLN(F("Weather JSON parse error"));  // Commented out to reduce log spam
    return false;
  }

  JsonObject o = doc["observations"][0];
  JsonObject m = o["metric"];

  *outT = m["temp"] | NAN;
  *outRH = o["humidity"] | NAN;

  if (isnan(*outT) || isnan(*outRH)) {
    // DEBUG_PRINTLN(F("Weather invalid"));  // Commented out to reduce log spam
    return false;
  }

  // DEBUG_PRINTLN(F("Weather OK"));  // Commented out to reduce log spam
  return true;
}

// =====================================================================
// ===================== CONFIG PERSISTENCE =============================
// =====================================================================
static void saveConfig() {
  g_prefs.begin("q150dew", false);
  g_prefs.putBool("heaterEn", g_cfg.heaterEnabled);
  g_prefs.putUChar("count", g_cfg.count);

  for (int i = 0; i < MAX_TABLE; i++) {
    g_prefs.putFloat(("sp" + String(i)).c_str(), g_cfg.table[i].spreadC);
    g_prefs.putUChar(("pw" + String(i)).c_str(), g_cfg.table[i].powerPct);
  }

  g_prefs.putString("ssid", g_cfg.wifiSSID);
  g_prefs.putString("pass", g_cfg.wifiPass);
  g_prefs.end();

  DEBUG_PRINTLN(F("Config saved"));
}

static void loadConfig() {
  g_prefs.begin("q150dew", true);

  g_cfg.heaterEnabled = g_prefs.getBool("heaterEn", true);
  g_cfg.count = constrain(g_prefs.getUChar("count", g_cfg.count), 1, MAX_TABLE);

  for (int i = 0; i < MAX_TABLE; i++) {
    g_cfg.table[i].spreadC =
      g_prefs.getFloat(("sp" + String(i)).c_str(), g_cfg.table[i].spreadC);
    g_cfg.table[i].powerPct =
      g_prefs.getUChar(("pw" + String(i)).c_str(), g_cfg.table[i].powerPct);
  }

  g_cfg.wifiSSID = g_prefs.getString("ssid", g_cfg.wifiSSID);
  g_cfg.wifiPass = g_prefs.getString("pass", g_cfg.wifiPass);
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

static void setupWiFiForWeather() {
  if (g_cfg.wifiSSID.isEmpty()) return;

  DEBUG_PRINT(F("WiFi connecting: "));
  DEBUG_PRINTLN(g_cfg.wifiSSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(g_cfg.wifiSSID.c_str(), g_cfg.wifiPass.c_str());

  // Wait up to 10 seconds for connection
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINT(F("WiFi connected: "));
    DEBUG_PRINTLN(WiFi.localIP());
  } else {
    DEBUG_PRINTLN(F("WiFi connection failed"));
  }
}

static void setupBLE() {
  DEBUG_PRINTLN(F("BLE start"));

  NimBLEDevice::init(BLE_DEVICE_NAME);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  
  NimBLEService* svc = server->createService(SVC_UUID);

  g_statusChar = svc->createCharacteristic(
    STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  auto configChar = svc->createCharacteristic(CONFIG_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  configChar->setCallbacks(new ConfigCallbacks());
  String initialConfig = encodeConfigJson();
  configChar->setValue(initialConfig);  // Set initial value
  DEBUG_PRINT(F("CONFIG initial value set: "));
  DEBUG_PRINTLN(initialConfig);

  auto infoChar = svc->createCharacteristic(
    INFO_UUID, NIMBLE_PROPERTY::READ);
  infoChar->setValue(encodeInfoJson());

  svc->createCharacteristic(CMD_UUID, NIMBLE_PROPERTY::WRITE);

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->start();

  DEBUG_PRINTLN(F("BLE ready"));
}

// =====================================================================
// ============================== LOOP ==================================
// =====================================================================
static void updateReadingsAndControl() {
  float T, H;

  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_PRINTLN(F("No WiFi"));
    return;
  }

  if (!fetchOutdoorWeather(&T, &H)) return;

  g_T = T;
  g_RH = H;
  g_Td = dewPointC(g_T, g_RH);
  g_spread = isnan(g_Td) ? NAN : (g_T - g_Td);

  g_powerPct = powerFromSpread(g_cfg, g_spread);
  pwmWritePercent(g_powerPct);

  // DEBUG_PRINT(F("Sp="));  // Commented out to reduce log spam
  // DEBUG_PRINT(g_spread);
  // DEBUG_PRINT(F(" P="));
  // DEBUG_PRINTLN(g_powerPct);
}

void setup() {
  DEBUG_BEGIN(115200);
  DEBUG_PRINTLN(F("Q150 Dew Controller"));
  DEBUG_PRINT(F("FW "));
  DEBUG_PRINTLN(FIRMWARE_VERSION);

  loadConfig();
  setupPWM();
  setupWiFiForWeather();
  setupBLE();
}

void loop() {
  static unsigned long lastRead = 0;
  static unsigned long lastNotify = 0;
  unsigned long now = millis();

  if (now - lastRead >= 5000) {
    lastRead = now;
    updateReadingsAndControl();
  }

  if (now - lastNotify >= 2000 && g_statusChar) {
    lastNotify = now;
    g_statusChar->setValue(encodeStatusJson());
    g_statusChar->notify();
  }

  delay(10);
}
