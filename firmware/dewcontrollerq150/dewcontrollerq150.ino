/***********************************************************************
 Q150 Dew Controller
 Board : Seeed XIAO ESP32S3
 Control: BLE + WiFi Web Interface
 Sensor : SHT45 (I2C)
 PWM    : D8 (Pin 9, Digital IO D8)
 I2C    : SDA=D4 (Pin 5), SCL=D5 (Pin 6)

 WiFi: Stored in config, falls back to AP mode if no credentials or connection fails
 Fallback AP SSID "Q150Dew", Password "tinka"

 Firmware Version: 1.3
************************************************************************/

#define FIRMWARE_VERSION "2.0"
#define DEBUG 1   // <<<<<<<<<< SET TO 0 FOR RELEASE BUILD

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
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>

// =====================================================================
// =========================== WIFI =====================================
// =====================================================================
// Fallback AP credentials (used when no WiFi configured or connection fails)
const char* FALLBACK_AP_SSID = "Q150Dew";
const char* FALLBACK_AP_PASSWORD = "tinka";

// NTP Configuration
const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";
bool ntpSynced = false;

WebServer server(80);
bool wifiConnected = false;
String wifiStatus = "Disconnected";
int wifiRSSI = 0;
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_RECONNECT_INTERVAL = 30000; // 30 seconds

// Stored WiFi credentials
String stored_ssid = "";
String stored_password = "";

// =====================================================================
// =========================== HARDWARE =================================
// =====================================================================
#define HEATER_PWM_PIN D8           // Pin 9, Digital IO D8 on XIAO ESP32S3
#define I2C_SDA D4                   // Pin 5, Digital IO D4
#define I2C_SCL D5                   // Pin 6, Digital IO D5
#define LED_PIN LED_BUILTIN          // Built-in LED for status indication

static constexpr int PWM_FREQ_HZ   = 1000;
static constexpr int PWM_RES_BITS  = 10;
static constexpr int PWM_MAX       = (1 << PWM_RES_BITS) - 1;

// Timing intervals (milliseconds)
static constexpr unsigned long SENSOR_READ_INTERVAL_MS = 5000;   // 5 seconds
static constexpr unsigned long STATUS_NOTIFY_INTERVAL_MS = 2000; // 2 seconds
static constexpr unsigned long WIFI_STATUS_CHECK_MS = 5000;      // 5 seconds

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
  char wifiSSID[32] = "";        // Store WiFi SSID
  char wifiPassword[64] = "";    // Store WiFi Password
  char timezone[64] = "AEST-10AEDT,M10.1.0,M4.1.0/3";     // POSIX timezone string (handles DST automatically)

  SpreadPowerEntry table[MAX_TABLE] = {
    {5.0f, 8},
    {3.0f, 15},
    {2.0f, 22},
    {1.0f, 30},
    {0.5f, 40}
  };
};

static Config g_cfg;
static Preferences g_prefs;

// Forward declarations
static void sortTableDescending(Config& cfg);
static uint8_t powerFromSpread(const Config& cfg, float spreadC);

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

// Log buffer for web interface
String logBuffer = "";
const int MAX_LOG_SIZE = 16384;  // 16KB log buffer

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
    addLog("BLE client connected");
  }
  
  void onDisconnect(BLEServer* pServer) {
    DEBUG_PRINTLN(F("❌ BLE disconnected — restarting advertising"));
    addLog("BLE client disconnected");
    BLEDevice::startAdvertising();
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

// =====================================================================
// ========================== NTP SETUP =================================
// =====================================================================
void setupNTP() {
  if (!wifiConnected) {
    addLog("⚠️ Cannot setup NTP - not connected to WiFi");
    return;
  }

  addLog("🕐 Configuring NTP with timezone: " + String(g_cfg.timezone));
  configTime(0, 0, NTP_SERVER1, NTP_SERVER2);
  setenv("TZ", g_cfg.timezone, 1);
  tzset();

  // Wait up to 5 seconds for time sync
  int attempts = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    delay(500);
    attempts++;
  }

  if (getLocalTime(&timeinfo)) {
    ntpSynced = true;
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    addLog("✅ NTP synced: " + String(timeStr));
  } else {
    ntpSynced = false;
    addLog("❌ NTP sync failed - time not available");
  }
}

String getCurrentTime() {
  if (!ntpSynced) return "--";
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "--";
  }
  
  char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStr);
}

// =====================================================================
// =========================== LOGGING ==================================
// =====================================================================
void addLog(const String& msg) {
  String timestamp;
  
  if (ntpSynced) {
    // Use real date-time when NTP is synced
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      char timeStr[32];
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
      timestamp = String(timeStr);
    } else {
      // Fallback to seconds if getLocalTime fails
      unsigned long ms = millis();
      timestamp = String(ms / 1000) + "s";
    }
  } else {
    // Use seconds since boot when NTP not synced
    unsigned long ms = millis();
    timestamp = String(ms / 1000) + "s";
  }
  
  String logLine = "[" + timestamp + "] " + msg;
  DEBUG_PRINTLN(logLine);
  
  logBuffer += logLine + "\n";
  if (logBuffer.length() > MAX_LOG_SIZE) {
    logBuffer = logBuffer.substring(logBuffer.length() - MAX_LOG_SIZE);
  }
}

// =====================================================================
// =========================== WIFI SETUP ===============================
// =====================================================================
void setupWiFi() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Check if we have stored credentials
  ///*
  if (strlen(g_cfg.wifiSSID) == 0) {
    addLog("⚠️ No WiFi credentials configured - starting in AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD);
    IPAddress apIP = WiFi.softAPIP();
    addLog("📡 AP mode started — connect to SSID '" + String(FALLBACK_AP_SSID) + "'");
    addLog("📶 AP IP address: " + apIP.toString());
    setupWebServer();
    return;
  }
  //*/
  stored_ssid = String(g_cfg.wifiSSID);
  stored_password = String(g_cfg.wifiPassword);
//  stored_ssid = String("AstroNet-2G");
//  stored_password = String("leanneannatinka");
  addLog("🔌 Connecting to Wi-Fi: " + stored_ssid);

  // --- Step 1: Clean disconnect and setup STA mode ---
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Stronger signal for tough connections

  // --- Step 2: Set hostname ---
  WiFi.setHostname("Q150Dew");

  // --- Step 3: Begin initial connection ---
  WiFi.begin(stored_ssid.c_str(), stored_password.c_str());
  delay(500); // short warm-up

  const int maxRetries = 60;  // total loop time ≈ 60s (increased for slow extender)
  int retries = 0;
  wl_status_t lastStatus = WL_IDLE_STATUS;

  // --- Step 4: Poll connection progress safely ---
  addLog("⏳ Waiting for Wi-Fi connection (can take up to 45s in extender mode)...");
  while (WiFi.status() != WL_CONNECTED && retries < maxRetries) {
    wl_status_t status = WiFi.status();

    // Log only when status changes
    if (status != lastStatus) {
      String statusMsg;
      switch (status) {
        case WL_IDLE_STATUS:      statusMsg = "Idle"; break;
        case WL_NO_SSID_AVAIL:    statusMsg = "No SSID available"; break;
        case WL_SCAN_COMPLETED:   statusMsg = "Scan completed"; break;
        case WL_CONNECTED:        statusMsg = "Connected"; break;
        case WL_CONNECT_FAILED:   statusMsg = "Connect failed"; break;
        case WL_CONNECTION_LOST:  statusMsg = "Connection lost"; break;
        case WL_DISCONNECTED:     statusMsg = "Disconnected"; break;
        default:                  statusMsg = "Unknown (" + String(status) + ")"; break;
      }
      addLog("⏳ Wi-Fi status: " + statusMsg);
      lastStatus = status;
    }

    delay(1000);
    retries++;
  }

  // --- Step 5: Handle success immediately ---
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH);
    wifiConnected = true;
    wifiStatus = "Connected to " + stored_ssid;
    wifiRSSI = WiFi.RSSI();
    addLog("✅ Wi-Fi connected");
    addLog("🌐 IP address: " + WiFi.localIP().toString());
    addLog("📶 RSSI: " + String(wifiRSSI) + " dBm");

    if (!MDNS.begin("q150dew")) {
      addLog("❌ mDNS failed to start!");
    } else {
      addLog("✅ mDNS responder started as q150dew.local");
    }

    setupNTP();
    setupWebServer();
    return;
  }

  // --- Step 6: Grace period for late DHCP (important for slow extender) ---
  addLog("⚠️ Connection not confirmed after " + String(maxRetries) + " attempts.");
  addLog("⏱ Waiting up to 15 more seconds for late DHCP...");

  unsigned long graceStart = millis();
  bool connected = false;
  while (millis() - graceStart < 15000) {
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      break;
    }
    delay(1000);
  }

  if (connected) {
    digitalWrite(LED_PIN, HIGH);
    wifiConnected = true;
    wifiStatus = "Connected to " + stored_ssid;
    wifiRSSI = WiFi.RSSI();
    addLog("✅ Late Wi-Fi success detected!");
    addLog("🌐 IP address: " + WiFi.localIP().toString());
    addLog("📶 RSSI: " + String(wifiRSSI) + " dBm");

    if (!MDNS.begin("q150dew")) {
      addLog("❌ mDNS failed to start!");
    } else {
      addLog("✅ mDNS responder started as q150dew.local");
    }

    setupNTP();
    setupWebServer();
    return;
  }

  // --- Step 7: Fallback to AP mode ---
  addLog("❌ Wi-Fi connection failed after extended wait. Switching to Access Point mode...");
  wifiConnected = false;
  wifiStatus = "AP Mode: " + String(FALLBACK_AP_SSID);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD);
  IPAddress apIP = WiFi.softAPIP();
  addLog("📡 AP mode started — connect to SSID '" + String(FALLBACK_AP_SSID) + "'");
  addLog("📶 AP IP address: " + apIP.toString());
  setupWebServer();
}

// =====================================================================
// ===================== WIFI STATUS CHECK ==============================
// =====================================================================
void checkWiFiStatus() {
  static unsigned long lastReconnectAttempt = 0;
  static unsigned long offlineSince = 0;
  static bool wasDisconnected = false;
  static unsigned long lastConnectionCheck = 0;
  static unsigned long lastScanCheck = 0;
  static bool scanning = false;

  // If we're in AP mode, periodically check if our configured network is available
  if (WiFi.getMode() == WIFI_AP) {
    wifiStatus = "AP Mode: " + String(FALLBACK_AP_SSID);
    wifiRSSI = -30; // Fake good signal for AP mode
    
    // Only try to scan if we have credentials
    if (strlen(g_cfg.wifiSSID) > 0) {
      unsigned long now = millis();
      
      // Scan for networks every 30 seconds when in AP mode
      if (now - lastScanCheck > 30000 && !scanning) {
        lastScanCheck = now;
        scanning = true;
        addLog("📡 AP mode: Scanning for configured network...");
        
        // Start scan in non-blocking mode
        WiFi.scanNetworks(true);
      }
      
      // Check if scan is complete
      if (scanning) {
        int scanResult = WiFi.scanComplete();
        if (scanResult >= 0) {
          // Scan complete
          scanning = false;
          
          // Check if our SSID is in the list
          for (int i = 0; i < scanResult; i++) {
            String foundSSID = WiFi.SSID(i);
            if (foundSSID.equals(String(g_cfg.wifiSSID))) {
              addLog("✅ Found configured network: " + foundSSID + " - attempting connection...");
              
              // Stop AP mode and try to connect
              WiFi.softAPdisconnect(true);
              WiFi.mode(WIFI_STA);
              WiFi.begin(g_cfg.wifiSSID, g_cfg.wifiPassword);
              break;
            }
          }
          WiFi.scanDelete();
        } else if (scanResult == -1) {
          // Scan still in progress
          return;
        } else if (scanResult == -2) {
          // Scan not triggered or failed
          scanning = false;
        }
      }
    }
    return;
  }

  // Force a connection check every second
  if (millis() - lastConnectionCheck > 1000) {
    lastConnectionCheck = millis();
    
    // Check if we're actually connected by sending a probe
    if (WiFi.status() == WL_CONNECTED) {
      // Even if WiFi.status says connected, verify we can reach the AP
      // by checking RSSI - if it's -127 or 0, we're not really connected
      int rssi = WiFi.RSSI();
      if (rssi == 0 || rssi == -127) {
        // Force disconnection
        addLog("⚠️ False connection detected (RSSI=" + String(rssi) + "), forcing reconnect");
        WiFi.disconnect();
        return;
      }
    }
  }

  // No credentials? Shouldn't be in STA mode, but just in case
  if (strlen(g_cfg.wifiSSID) == 0) {
    return;
  }

  wl_status_t status = WiFi.status();

  // ---- Case 1: STA is connected ----
  if (status == WL_CONNECTED) {
    if (wasDisconnected) {
      wasDisconnected = false;
      offlineSince = 0;

      wifiConnected = true;
      wifiStatus = "Connected to " + stored_ssid;
      wifiRSSI = WiFi.RSSI();
      addLog("✅ Wi-Fi reconnected: " + WiFi.localIP().toString());

      delay(500); // give stack time to stabilise

      if (!MDNS.begin("q150dew")) {
        addLog("❌ Failed to restart mDNS responder!");
      } else {
        addLog("✅ mDNS responder restarted as q150dew.local");
      }

      // Restart web server to bind to new interface
      server.stop();
      delay(200);
      setupNTP();
      setupWebServer();
      addLog("🌐 Web server rebound to Wi-Fi interface");
    }
    
    // Update RSSI even when connected
    wifiRSSI = WiFi.RSSI();
    return;
  }

  // ---- Case 2: Not connected ----
  if (!wasDisconnected) {
    wasDisconnected = true;
    offlineSince = millis();
    wifiConnected = false;
    wifiStatus = "Disconnected";
    addLog("⚠️ Wi-Fi disconnected, will attempt reconnect...");
  }

  unsigned long now = millis();
  const unsigned long reconnectIntervalMs = 10000; // 10 seconds
  if (now - lastReconnectAttempt < reconnectIntervalMs) return;
  lastReconnectAttempt = now;

  // Check current status - don't interrupt if connection is in progress
  if (status == WL_IDLE_STATUS || status == WL_DISCONNECTED) {
    // Connection attempt finished but failed - safe to retry
    addLog("🔁 Retrying WiFi connection...");
    WiFi.begin(g_cfg.wifiSSID, g_cfg.wifiPassword);
  }
  else if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) {
    // Hard failure states - only after prolonged offline do a full reset
    const unsigned long offlineResetMs = 90000; // 90s
    if (offlineSince && (now - offlineSince > offlineResetMs)) {
      addLog("🧩 WiFi offline for >90s — performing hard reset...");
      WiFi.disconnect(true, true);
      delay(200);
      WiFi.mode(WIFI_STA);
      WiFi.setHostname("Q150Dew");
      WiFi.begin(g_cfg.wifiSSID, g_cfg.wifiPassword);
      offlineSince = now;  // Reset timer after hard reset
    } else {
      // Try again without disrupting the stack
      addLog("🔁 Retrying after connection failure...");
      WiFi.begin(g_cfg.wifiSSID, g_cfg.wifiPassword);
    }
  }
  // else: status is WL_SCAN_COMPLETED or other transient state - let it continue
}

// =====================================================================
// ===================== LED STATUS INDICATION ==========================
// =====================================================================
void handleLEDStatus() {
  static unsigned long lastLED = 0;
  static bool ledState = false;
  static int currentLedMode = -1;

  if (WiFi.getMode() == WIFI_AP) {
    // AP mode: slow blink
    if (currentLedMode != 2) {
      currentLedMode = 2;
      lastLED = millis();
      ledState = false;
      digitalWrite(LED_PIN, ledState);
    }
    if (millis() - lastLED > 500) {
      lastLED = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    // Connected: solid ON
    if (currentLedMode != 1) {
      currentLedMode = 1;
      digitalWrite(LED_PIN, HIGH);
    }
  } else {
    // Disconnected: OFF
    if (currentLedMode != 0) {
      currentLedMode = 0;
      digitalWrite(LED_PIN, LOW);
    }
  }
}

// =====================================================================
// ======================== WEB SERVER ==================================
// =====================================================================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Q150 Dew Controller</title>
  <style>
    * { box-sizing: border-box; }
    body { 
      font-family: Arial, sans-serif; 
      margin: 0; 
      padding: 10px; 
      background: #f5f5f5;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      overflow-x: hidden;
      overflow-y: auto;
    }
    .container { 
      display: flex;
      flex-direction: column;
      height: 100%;
      max-width: 1400px;
      margin: 0 auto;
      width: 100%;
    }
    .header { 
      background: #2c3e50; 
      color: white; 
      padding: 8px 15px; 
      border-radius: 8px 8px 0 0; 
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-shrink: 0;
    }
    .header h1 { 
      margin: 0; 
      font-size: 1.3rem;
    }
    .header p { 
      margin: 0; 
      font-size: 0.9rem;
      color: #ecf0f1;
    }
    .connection-status {
      display: inline-block;
      padding: 3px 10px;
      border-radius: 12px;
      font-size: 0.75rem;
      font-weight: bold;
      margin-left: 10px;
    }
    .connection-status.connected {
      background: #27ae60;
      color: white;
    }
    .connection-status.disconnected {
      background: #e74c3c;
      color: white;
    }
    .main-content {
      display: flex;
      flex: 1;
      min-height: 0;
      gap: 10px;
      padding: 10px 0;
      width: 100%;
    }
    
    .left-panel {
      flex: 1;
      display: flex;
      flex-direction: column;
      gap: 10px;
      min-width: 0;
      height: 100%;
    }
    
    .right-panel {
      flex: 1;
      display: flex;
      flex-direction: column;
      gap: 10px;
      min-width: 0;
      height: 100%;
    }
    
    .panel {
      background: white;
      border-radius: 8px;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
      padding: 12px;
      overflow: visible;
      flex-shrink: 0;
    }
    
    /* Make the status panel taller */
    .left-panel .panel:first-child {
      flex: 2;
    }
    
    .left-panel .panel:last-child {
      flex: 1;
    }
    
    /* Make the right panel panels evenly distributed */
    .right-panel .panel {
      flex: 1;
    }
    
    .panel h3 {
      margin: 0 0 10px 0;
      font-size: 1rem;
      color: #2c3e50;
      border-bottom: 1px solid #eee;
      padding-bottom: 5px;
    }
    
    /* Vertical status layout */
    .status-vertical {
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    .status-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 6px 8px;
      background: #f8f9fa;
      border-radius: 6px;
      font-size: 0.9rem;
    }
    .status-label {
      color: #2c3e50;
      font-weight: 500;
    }
    .status-value {
      font-weight: bold;
      color: #3498db;
      font-family: monospace;
      font-size: 1.1rem;
    }
    
    .wifi-status {
      background: #e8f4fd;
      padding: 6px 10px;
      border-radius: 5px;
      font-size: 0.8rem;
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 10px;
    }
    .heater-on { 
      background: #ffebee; 
      color: #c0392b; 
      padding: 2px 8px; 
      border-radius: 12px; 
      font-weight: bold;
      font-size: 0.8rem;
    }
    .heater-off { 
      background: #e8f5e9; 
      color: #27ae60; 
      padding: 2px 8px; 
      border-radius: 12px; 
      font-weight: bold;
      font-size: 0.8rem;
    }
    
    button { 
      background: #3498db; 
      color: white; 
      border: none; 
      padding: 6px 12px; 
      border-radius: 4px; 
      cursor: pointer; 
      margin: 2px;
      font-size: 0.8rem;
    }
    button:hover { background: #2980b9; }
    button.danger { background: #e74c3c; }
    button.danger:hover { background: #c0392b; }
    button.success { background: #27ae60; }
    button.success:hover { background: #229954; }
    
    .button-row {
      display: flex;
      flex-wrap: wrap;
      gap: 5px;
      margin: 8px 0;
    }
    
    /* Spread Table Styles */
    .spread-table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.8rem;
      margin-bottom: 8px;
    }
    .spread-table th {
      background: #ecf0f1;
      padding: 6px;
      text-align: center;
    }
    .spread-table td {
      padding: 4px;
      text-align: center;
    }
    .spread-table input {
      width: 70px;  /* Slightly smaller to fit in 40% panel */
      padding: 4px;
      border: 1px solid #ddd;
      border-radius: 3px;
      text-align: center;
    }
    
    /* WiFi Config Styles */
    .config-section {
      background: #f8f9fa;
      padding: 8px;  /* Slightly smaller padding */
      border-radius: 6px;
    }
    .config-row {
      display: flex;
      align-items: center;
      margin: 4px 0;  /* Reduced margin */
      gap: 6px;  /* Reduced gap */
    }
    .config-row label {
      width: 65px;  /* Slightly smaller */
      font-size: 0.75rem;  /* Smaller font */
      color: #555;
    }
    .config-input {
      flex: 1;
      padding: 4px;  /* Smaller padding */
      border: 1px solid #ddd;
      border-radius: 4px;
      font-size: 0.75rem;  /* Smaller font */
    }
    
    /* Log Panel */
    .log-container {
      background: white;
      border-radius: 8px;
      box-shadow: 0 2px 4px rgba(0,0,0,0.1);
      padding: 12px;
      display: flex;
      flex-direction: column;
      flex: 1;
      min-height: 0;
      margin-top: 10px;
      overflow: hidden;
    }
    .log-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 10px;
    }
    .log-header h3 {
      margin: 0;
      font-size: 1rem;
      color: #2c3e50;
      border-bottom: none;
      padding-bottom: 0;
    }
    .log-panel { 
      background: #1e1e1e; 
      color: #00ff00; 
      padding: 10px; 
      border-radius: 5px; 
      font-family: monospace; 
      font-size: 0.8rem;
      flex: 1;
      overflow-y: auto; 
      white-space: pre-wrap;
      word-break: break-all;
      min-height: 100px;
      max-height: 300px;
    }
    
    .footer {
      text-align: center;
      font-size: 0.7rem;
      color: #7f8c8d;
      padding: 5px;
      flex-shrink: 0;
    }
    
    /* Mobile Responsive Styles */
    @media (max-width: 768px) {
      body {
        padding: 5px;
      }
      
      .container {
        height: auto;
        min-height: 100vh;
      }
      
      .main-content {
        flex-direction: column;
        padding: 5px 0;
      }
      
      .left-panel, .right-panel {
        width: 100%;
        height: auto;
      }
      
      .panel {
        padding: 10px;
      }
      
      .left-panel .panel:first-child,
      .left-panel .panel:last-child,
      .right-panel .panel {
        flex: none;
      }
      
      .log-container {
        margin-top: 5px;
        margin-bottom: 10px;
        min-height: 200px;
      }
      
      .log-panel {
        max-height: 250px;
        font-size: 0.7rem;
      }
      
      .header h1 {
        font-size: 1.1rem;
      }
      
      .header p {
        font-size: 0.8rem;
      }
      
      .status-row {
        font-size: 0.85rem;
      }
      
      .status-value {
        font-size: 1rem;
      }
    }
    
    /* iPhone specific adjustments */
    @media (max-width: 430px) {
      body {
        padding: 3px;
      }
      
      .header {
        padding: 6px 10px;
      }
      
      .header h1 {
        font-size: 1rem;
      }
      
      .header p {
        font-size: 0.75rem;
      }
      
      .panel {
        padding: 8px;
      }
      
      .status-row {
        padding: 5px 6px;
        font-size: 0.8rem;
      }
      
      .status-value {
        font-size: 0.95rem;
      }
      
      button {
        padding: 5px 10px;
        font-size: 0.75rem;
      }
      
      .log-container {
        padding: 8px;
        margin-bottom: 15px;
      }
      
      .log-panel {
        font-size: 0.65rem;
      }
    }
  </style>
  
  <script>
    let logPaused = false;
    let statusData = {};
    let isConnected = false;
    let failedAttempts = 0;
    const MAX_FAILED_ATTEMPTS = 2;  // Show disconnected after 2 failed attempts

    function updateConnectionStatus() {
      const statusElem = document.getElementById('connection-status');
      if (statusElem) {
        if (isConnected) {
          statusElem.className = 'connection-status connected';
          statusElem.innerText = '● Online';
        } else {
          statusElem.className = 'connection-status disconnected';
          statusElem.innerText = '● Offline';
        }
      }
    }

    function updateStatus() {
      // Create abort controller for timeout
      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 1000); // 1 second timeout
      
      fetch('/api/status', { signal: controller.signal })
        .then(response => {
          clearTimeout(timeoutId);
          if (!response.ok) {
            throw new Error('Network response was not ok');
          }
          return response.json();
        })
        .then(data => {
          statusData = data;
          
          // Mark as connected
          if (!isConnected) {
            isConnected = true;
            failedAttempts = 0;
            updateConnectionStatus();
          }
          
          // Update status values
          document.getElementById('temp-value').innerText = data.T.toFixed(1) + ' °C';
          document.getElementById('humidity-value').innerText = data.RH.toFixed(1) + ' %';
          document.getElementById('dewpoint-value').innerText = data.Td.toFixed(1) + ' °C';
          document.getElementById('spread-value').innerText = data.spread.toFixed(1) + ' °C';
          document.getElementById('power-value-display').innerText = data.power + ' %';
          
          // Update heater state
          const heaterElem = document.getElementById('heater-state');
          heaterElem.innerText = data.enabled ? 'ON' : 'OFF';
          heaterElem.className = data.enabled ? 'heater-on' : 'heater-off';
          
          // Update toggle heater button color
          const toggleBtn = document.getElementById('toggle-heater-btn');
          if (toggleBtn) {
            toggleBtn.innerText = 'Toggle Heater'; // Restore text when connected
            if (!data.enabled) {
              // Heater disabled → red
              toggleBtn.style.background = '#e74c3c';
            } else if (data.power === 0) {
              // Heater enabled but power 0% → orange
              toggleBtn.style.background = '#ff8c00';
            } else {
              // Heater enabled and power > 0% → green
              toggleBtn.style.background = '#27ae60';
            }
          }
          
          // Update WiFi info with color based on connection status
          const wifiInfo = document.getElementById('wifi-info');
          wifiInfo.innerHTML = `<span>📡 ${data.wifiStatus}</span> <span>${data.wifiRSSI} dBm</span>`;
          
          // Check if WiFi is actually disconnected (status contains "Disconnected")
          if (data.wifiStatus.includes('Disconnected')) {
            wifiInfo.style.backgroundColor = '#ffebee'; // Light red for disconnected
            wifiInfo.style.color = '#b71c1c'; // Dark red text
          } else {
            wifiInfo.style.backgroundColor = '#e8f5e9'; // Light green for connected
            wifiInfo.style.color = '#1e4620'; // Dark green text
          }
          
          // Update time display
          const timeValue = document.getElementById('time-value');
          if (timeValue) {
            if (data.ntpSynced && data.time !== '--') {
              timeValue.innerText = data.time;
            } else {
              timeValue.innerText = 'Not synced';
            }
          }
          
          // Update WiFi fields - ONLY if neither is currently being edited
          const ssidField = document.getElementById('wifi-ssid');
          const passField = document.getElementById('wifi-password');
          const tzField = document.getElementById('wifi-timezone');
          const wifiFieldHasFocus = (document.activeElement === ssidField || document.activeElement === passField || document.activeElement === tzField);
          
          if (ssidField && !wifiFieldHasFocus) {
            ssidField.value = data.wifiSSID || '';
          }
          
          if (tzField && !wifiFieldHasFocus) {
            tzField.value = data.timezone || 'AEST-10AEDT,M10.1.0,M4.1.0/3';
          }
          
          if (passField && !wifiFieldHasFocus) {
            if (data.wifiPasswordLen && data.wifiPasswordLen > 0) {
              passField.value = '*'.repeat(data.wifiPasswordLen);
            } else {
              passField.value = '';
              passField.placeholder = 'Enter password';
            }
          }
        })
        .catch(error => {
          console.log('Status update failed:', error);
          
          // Increment failed attempts
          failedAttempts++;
          if (failedAttempts >= MAX_FAILED_ATTEMPTS && isConnected) {
            isConnected = false;
            updateConnectionStatus();
          }
          
          // Keep WiFi info showing last known state - don't clear it
          // This allows distinguishing between "WiFi connected but device offline" 
          // vs "No WiFi connection" scenarios
          
          // Clear time display
          const timeValue = document.getElementById('time-value');
          if (timeValue) {
            timeValue.innerText = '--';
          }
          
          // Clear sensor readings
          document.getElementById('temp-value').innerText = '-- °C';
          document.getElementById('humidity-value').innerText = '-- %';
          document.getElementById('dewpoint-value').innerText = '-- °C';
          document.getElementById('spread-value').innerText = '-- °C';
          document.getElementById('power-value-display').innerText = '-- %';
          
          // Set heater to unknown state
          const heaterElem = document.getElementById('heater-state');
          heaterElem.innerText = '?';
          heaterElem.className = '';
          
          // Update toggle heater button to gray when disconnected
          const toggleBtn = document.getElementById('toggle-heater-btn');
          if (toggleBtn) {
            toggleBtn.style.background = '#757575'; // Gray for disconnected
          }
        });
    }

    function updateLog() {
      if (logPaused) return;
      
      // Create abort controller for timeout
      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 1000); // 1 second timeout
      
      fetch('/api/log', { signal: controller.signal })
        .then(response => {
          clearTimeout(timeoutId);
          return response.text();
        })
        .then(data => {
          const logElem = document.getElementById('log');
          logElem.innerText = data;
          logElem.scrollTop = logElem.scrollHeight;
        })
        .catch(error => console.log('Log update failed:', error));
    }
    
    function toggleHeater() {
      fetch('/api/toggle', { method: 'POST' })
        .then(() => setTimeout(updateStatus, 100));
    }
    
    function setManualPower(power) {
      fetch('/api/power', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'power=' + power
      }).then(() => {
        // Update status after command
        setTimeout(updateStatus, 100);
      }).catch(error => {
        console.log('Power command failed:', error);
      });
    }
    
    function updatePowerValue(val) {
      document.getElementById('manual-power-label').innerText = val + '%';
      // Send power command immediately as slider moves
      setManualPower(val);
    }
    
    function saveWiFiConfig() {
      const ssid = document.getElementById('wifi-ssid').value;
      const password = document.getElementById('wifi-password').value;
      const timezone = document.getElementById('wifi-timezone').value;
      
      if (!ssid) {
        alert('Please enter an SSID');
        return;
      }
      
      fetch('/api/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password) + '&timezone=' + encodeURIComponent(timezone)
      })
      .then(response => {
        if (response.ok) {
          alert('WiFi credentials saved. Device will restart to connect...');
          setTimeout(() => { window.location.reload(); }, 2000);
        } else {
          return response.text().then(text => {
            alert('Error: ' + text);
          });
        }
      })
      .catch(error => {
        alert('Failed to save WiFi credentials');
      });
    }
    
    function saveSpreadTable() {
      const table = [];
      for (let i = 0; i < 5; i++) {
        const spread = parseFloat(document.getElementById('spread' + i).value);
        const power = parseInt(document.getElementById('power' + i).value);
        if (!isNaN(spread) && !isNaN(power)) {
          table.push({spreadC: spread, powerPct: power});
        }
      }
      
      const config = {
        heaterEnabled: statusData ? statusData.enabled : false,
        table: table
      };
      
      fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config)
      })
      .then(response => {
        if (response.ok) {
          alert('Spread table saved');
          setTimeout(updateStatus, 500);
        }
      })
      .catch(error => {
        alert('Failed to save spread table');
      });
    }
    
    function toggleLogPause() {
      logPaused = !logPaused;
      document.getElementById('pause-log-btn').innerText = logPaused ? 'Resume Log' : 'Pause Log';
      if (!logPaused) updateLog();
    }
    
    function clearWiFiFields() {
      document.getElementById('wifi-ssid').value = '';
      document.getElementById('wifi-password').value = '';
      document.getElementById('wifi-timezone').value = 'AEST-10AEDT,M10.1.0,M4.1.0/3';
    }
    
    // Update status every 2 seconds
    setInterval(updateStatus, 2000);
    
    // Update log every 5 seconds
    setInterval(updateLog, 5000);
    
    window.onload = function() {
      updateStatus();
      updateLog();
    };
  </script>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>Q150 Dew Controller<span id="connection-status" class="connection-status disconnected">● Offline</span></h1>
      <p>Firmware v)rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(</p>
    </div>
    
    <div class="main-content">
      <!-- Left Panel - Vertical Status -->
      <div class="left-panel">
        <!-- WiFi Status -->
        <div class="panel">
          <div class="wifi-status" id="wifi-info">
            <span>📡 Loading...</span>
            <span></span>
          </div>
          <div class="wifi-status" id="time-info" style="margin-top: 5px;">
            <span>🕐 Time</span>
            <span id="time-value">--</span>
          </div>
          
          <h3>Current Readings</h3>
          <div class="status-vertical">
            <div class="status-row">
              <span class="status-label">🌡️ Temperature</span>
              <span class="status-value" id="temp-value">-- °C</span>
            </div>
            <div class="status-row">
              <span class="status-label">💧 Humidity</span>
              <span class="status-value" id="humidity-value">-- %</span>
            </div>
            <div class="status-row">
              <span class="status-label">❄️ Dew Point</span>
              <span class="status-value" id="dewpoint-value">-- °C</span>
            </div>
            <div class="status-row">
              <span class="status-label">📊 Spread</span>
              <span class="status-value" id="spread-value">-- °C</span>
            </div>
            <div class="status-row">
              <span class="status-label">⚡ Heater Power</span>
              <span class="status-value" id="power-value-display">-- %</span>
            </div>
            <div class="status-row">
              <span class="status-label">🔌 Heater</span>
              <span class="status-value"><span id="heater-state" class="heater-off">OFF</span></span>
            </div>
          </div>
        </div>
        
        <!-- Quick Controls -->
        <div class="panel">
          <h3>Controls</h3>
          <div class="button-row">
            <button id="toggle-heater-btn" onclick="toggleHeater()">Toggle Heater</button>
            <button id="pause-log-btn" onclick="toggleLogPause()">Pause Log</button>
          </div>
          
          <div style="margin-top: 10px;">
            <div style="display: flex; align-items: center; gap: 8px;">
              <span style="font-size:0.8rem; min-width: 60px;">Manual:</span>
              <input type="range" style="flex:1;" id="manual-power" min="0" max="100" value="0" oninput="updatePowerValue(this.value)">
              <span id="manual-power-label" style="font-size:0.8rem; min-width:40px;">0%</span>
              <!-- Set button removed -->
            </div>
          </div>
        </div>
      </div>
      
      <!-- Right Panel - Power Table (top) and WiFi (bottom) -->
      <div class="right-panel">
        <!-- Power Table (now on top) -->
        <div class="panel">
          <h3>Spread → Power Table</h3>
          <table class="spread-table">
            <thead>
              <tr><th>Spread ≤ (°C)</th><th>Power %</th></tr>
            </thead>
            <tbody>
              <tr><td><input type="number" step="0.1" id="spread0" value="5.0"></td><td><input type="number" id="power0" value="8"></td></tr>
              <tr><td><input type="number" step="0.1" id="spread1" value="3.0"></td><td><input type="number" id="power1" value="15"></td></tr>
              <tr><td><input type="number" step="0.1" id="spread2" value="2.0"></td><td><input type="number" id="power2" value="22"></td></tr>
              <tr><td><input type="number" step="0.1" id="spread3" value="1.0"></td><td><input type="number" id="power3" value="30"></td></tr>
              <tr><td><input type="number" step="0.1" id="spread4" value="0.5"></td><td><input type="number" id="power4" value="40"></td></tr>
            </tbody>
          </table>
          <div style="text-align: right; margin-top: 8px;">
            <button class="success" onclick="saveSpreadTable()">Save Spread Table</button>
          </div>
        </div>
        
        <!-- WiFi Configuration (now below power table) -->
        <div class="panel">
          <h3>WiFi Configuration</h3>
          <div class="config-section">
            <div class="config-row">
              <label>SSID:</label>
              <input type="text" id="wifi-ssid" class="config-input" placeholder="Enter SSID" value="">
            </div>
            <div class="config-row">
              <label>Password:</label>
              <input type="password" id="wifi-password" class="config-input" placeholder="Enter password">
            </div>
            <div class="config-row">
              <label>Timezone:</label>
              <select id="wifi-timezone" class="config-input">
                <option value="UTC0">UTC (GMT)</option>
                <option value="GMT0BST,M3.5.0/1,M10.5.0">UK (London)</option>
                <option value="CET-1CEST,M3.5.0,M10.5.0/3">Europe (Paris/Berlin)</option>
                <option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Europe East (Athens)</option>
                <option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option>
                <option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
                <option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option>
                <option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option>
                <option value="AKST9AKDT,M3.2.0,M11.1.0">US Alaska</option>
                <option value="HST10">US Hawaii</option>
                <option value="AEST-10AEDT,M10.1.0,M4.1.0/3" selected>Australia (Sydney/Melbourne)</option>
                <option value="ACST-9:30ACDT,M10.1.0,M4.1.0/3">Australia (Adelaide)</option>
                <option value="AWST-8">Australia (Perth)</option>
                <option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand</option>
                <option value="JST-9">Japan</option>
                <option value="CST-8">China</option>
                <option value="IST-5:30">India</option>
                <option value="<+03>-3">Middle East (Saudi)</option>
              </select>
            </div>
            <div style="display: flex; gap: 5px; margin-top: 8px;">
              <button class="success" onclick="saveWiFiConfig()" style="flex: 1;">Save & Restart</button>
              <button onclick="clearWiFiFields()" style="flex: 0;">Clear</button>
            </div>
            <p style="font-size: 0.7rem; color: #666; margin: 5px 0 0 0;">
              ℹ️ Falls back to AP "Q150Dew" if connection fails
            </p>
          </div>
        </div>
      </div>
    </div>
    
    <!-- Log Panel - Full width, expands with window -->
    <div class="log-container">
      <div class="log-header">
        <h3>System Log</h3>
        <span style="font-size:0.7rem;">updates every 5s</span>
      </div>
      <div id="log" class="log-panel">Loading...</div>
    </div>
    
    <div class="footer">
      Q150 Dew Controller • Built for Skywatcher Quattro 150P
    </div>
  </div>
</body>
</html>
  )rawliteral";
  
  server.send(200, "text/html", html);
}

// Add new endpoint for saving spread table
void handleAPIConfig() {
  addLog("📥 API: /api/config");
  if (server.method() == HTTP_POST) {
    String body = server.arg("plain");
    StaticJsonDocument<512> doc;
    auto err = deserializeJson(doc, body);
    
    if (!err) {
      if (doc.containsKey("table")) {
        JsonArray table = doc["table"];
        g_cfg.count = min((int)table.size(), MAX_TABLE);
        
        for (int i = 0; i < g_cfg.count; i++) {
          JsonObject entry = table[i];
          g_cfg.table[i].spreadC = entry["spreadC"] | 0.0f;
          g_cfg.table[i].powerPct = entry["powerPct"] | 0;
        }
        sortTableDescending(g_cfg);
        saveConfig();
        addLog("📊 Spread table updated");
        server.send(200, "text/plain", "OK");
        return;
      }
    }
  }
  server.send(400, "text/plain", "Invalid request");
}

void handleAPIStatus() {
  StaticJsonDocument<384> doc;
  doc["T"] = g_T;
  doc["RH"] = g_RH;
  doc["Td"] = g_Td;
  doc["spread"] = g_spread;
  doc["power"] = g_powerPct;
  doc["enabled"] = g_cfg.heaterEnabled;
  doc["manual"] = g_manual_mode;
  doc["manualPower"] = g_manual_power;
  doc["wifiStatus"] = wifiStatus;
  doc["wifiRSSI"] = wifiRSSI;
  doc["wifiSSID"] = String(g_cfg.wifiSSID);
  doc["wifiPasswordLen"] = strlen(g_cfg.wifiPassword);
  doc["timezone"] = String(g_cfg.timezone);
  doc["version"] = FIRMWARE_VERSION;
  doc["time"] = getCurrentTime();
  doc["ntpSynced"] = ntpSynced;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleAPILog() {
  server.send(200, "text/plain", logBuffer);
}

void handleAPIToggle() {
  addLog("📥 API: /api/toggle");
  g_cfg.heaterEnabled = !g_cfg.heaterEnabled;
  saveConfig();
  addLog("Heater toggled: " + String(g_cfg.heaterEnabled ? "ON" : "OFF"));
  server.send(200, "text/plain", "OK");
}

void handleAPIPower() {
  if (server.hasArg("power")) {
    String powerStr = server.arg("power");
    int power = powerStr.toInt();
    power = constrain(power, 0, 100);
    addLog("📥 API: /api/power = " + String(power) + "%");
  
    // Set manual mode and power
    g_manual_mode = true;
    g_manual_power = power;
    g_powerPct = power;
    pwmWritePercent(power);
    
    // Send success response
    server.send(200, "text/plain", "OK");
    
    // Force a status update notification
    String status = encodeStatusJson();
    if (g_statusChar) {
      g_statusChar->setValue(status.c_str());
      g_statusChar->notify();
    }
  } else {
    server.send(400, "text/plain", "Missing power parameter");
  }
}


void handleAPIWiFi() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    String newTimezone = "UTC0";
    if (server.hasArg("timezone")) {
      newTimezone = server.arg("timezone");
    }
    addLog("📥 API: /api/wifi - SSID: " + newSSID + ", Password: *******, Timezone: " + newTimezone);
    
    // Update config
    strlcpy(g_cfg.wifiSSID, newSSID.c_str(), sizeof(g_cfg.wifiSSID));
    strlcpy(g_cfg.wifiPassword, newPassword.c_str(), sizeof(g_cfg.wifiPassword));
    strlcpy(g_cfg.timezone, newTimezone.c_str(), sizeof(g_cfg.timezone));
    
    saveConfig();
    addLog("📡 WiFi credentials updated - restarting...");
    server.send(200, "text/plain", "OK");
    
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID or password");
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/api/status", handleAPIStatus);
  server.on("/api/log", handleAPILog);
  server.on("/api/toggle", HTTP_POST, handleAPIToggle);
  server.on("/api/power", HTTP_POST, handleAPIPower);
  server.on("/api/wifi", HTTP_POST, handleAPIWiFi);
  server.on("/api/config", HTTP_POST, handleAPIConfig);

  server.begin();
  addLog("🌐 Web server started");
  
  if (wifiConnected) {
    addLog("📱 Access at: http://" + WiFi.localIP().toString());
    addLog("📱 Or: http://q150dew.local");
  } else {
    addLog("📱 Access at: http://" + WiFi.softAPIP().toString());
  }
}

// =====================================================================
// ===================== INFO JSON ============================
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
  StaticJsonDocument<768> doc;
  doc[F("cfgVersion")] = g_cfg.cfgVersion;
  doc[F("heaterEnabled")] = g_cfg.heaterEnabled;
  doc[F("wifiSSID")] = g_cfg.wifiSSID;
  doc[F("wifiPassword")] = "********"; // Don't send actual password
  
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
  StaticJsonDocument<768> doc;
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
  
  // WiFi credentials (only update if provided and not placeholder)
  if (doc.containsKey(F("wifiSSID")) && doc.containsKey(F("wifiPassword"))) {
    const char* newSSID = doc[F("wifiSSID")];
    const char* newPassword = doc[F("wifiPassword")];
    
    // Only update if not the placeholder and different from current
    if (strcmp(newPassword, "********") != 0) {
      if (strcmp(newSSID, g_cfg.wifiSSID) != 0 || strcmp(newPassword, g_cfg.wifiPassword) != 0) {
        strlcpy(g_cfg.wifiSSID, newSSID, sizeof(g_cfg.wifiSSID));
        strlcpy(g_cfg.wifiPassword, newPassword, sizeof(g_cfg.wifiPassword));
        DEBUG_PRINTLN(F("📡 WiFi credentials updated"));
      }
    }
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
    addLog("BLE disconnected");
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
  g_prefs.putString("wifiSSID", String(g_cfg.wifiSSID));
  g_prefs.putString("wifiPass", String(g_cfg.wifiPassword));
  g_prefs.putString("timezone", String(g_cfg.timezone));

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
  
  String ssid = g_prefs.getString("wifiSSID", "");
  String pass = g_prefs.getString("wifiPass", "");
  strlcpy(g_cfg.wifiSSID, ssid.c_str(), sizeof(g_cfg.wifiSSID));
  strlcpy(g_cfg.wifiPassword, pass.c_str(), sizeof(g_cfg.wifiPassword));
  
  String tz = g_prefs.getString("timezone", "AEST-10AEDT,M10.1.0,M4.1.0/3");
  strlcpy(g_cfg.timezone, tz.c_str(), sizeof(g_cfg.timezone));

  for (int i = 0; i < MAX_TABLE; i++) {
    g_cfg.table[i].spreadC =
      g_prefs.getFloat(("sp" + String(i)).c_str(), g_cfg.table[i].spreadC);
    g_cfg.table[i].powerPct =
      g_prefs.getUChar(("pw" + String(i)).c_str(), g_cfg.table[i].powerPct);
  }

  g_prefs.end();

  sortTableDescending(g_cfg);

  DEBUG_PRINTLN(F("Config loaded"));
  if (strlen(g_cfg.wifiSSID) > 0) {
    DEBUG_PRINT("WiFi SSID: ");
    DEBUG_PRINTLN(g_cfg.wifiSSID);
  }
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
    addLog("❌ SHT45 sensor not found");
    return;
  }
  
  sht4.setPrecision(SHT4X_HIGH_PRECISION);
  sht4.setHeater(SHT4X_NO_HEATER);
  DEBUG_PRINTLN(F("SHT45 ready"));
  addLog("✅ SHT45 sensor initialized");
}

static void setupBLE() {
  DEBUG_PRINTLN(F("BLE start"));
  addLog("📡 Starting BLE...");

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

  g_infoChar = svc->createCharacteristic(
    INFO_UUID,
    BLECharacteristic::PROPERTY_READ);

  String initialInfo = encodeInfoJson();
  g_infoChar->setValue(initialInfo.c_str());

  g_cmdChar = svc->createCharacteristic(
    CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR);
  g_cmdChar->setCallbacks(&cmdCallbacks);

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();

  BLEAdvertisementData advData;
  advData.setName(BLE_DEVICE_NAME);
  advData.setCompleteServices(BLEUUID(SVC_UUID));

  adv->setAdvertisementData(advData);
  adv->setScanResponse(false);

  BLEDevice::startAdvertising();

  DEBUG_PRINTLN(F("BLE ready"));
  addLog("✅ BLE ready - Device name: " + String(BLE_DEVICE_NAME));
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

  // Initialize log
  addLog("🚀 Q150 Dew Controller v" + String(FIRMWARE_VERSION));
  
  loadConfig();
  setupPWM();
  setupSensor();
  setupBLE();
  
  // Setup WiFi and Web Server
  setupWiFi();
  setupWebServer();
  
  // Set LED to indicate WiFi mode
  pinMode(LED_PIN, OUTPUT);
  if (wifiConnected) {
    digitalWrite(LED_PIN, HIGH); // Solid ON for connected
  } else {
    // Will blink in loop for AP mode
  }
}

void loop() {
  static unsigned long lastRead = 0;
  static unsigned long lastNotify = 0;
  static unsigned long lastConfigUpdate = 0;
  static unsigned long lastWiFiStatusCheck = 0;
  static bool ledState = false;
  unsigned long now = millis();

  // Process pending BLE operations FIRST (minimal latency)
  processPendingConfigWrite();
  processPendingCmdWrite();

  // Check BLE connection
  bleWatchdog();

  // Handle web server clients
  server.handleClient();

  // Check WiFi status periodically
  if (now - lastWiFiStatusCheck >= WIFI_STATUS_CHECK_MS) {
    lastWiFiStatusCheck = now;
    checkWiFiStatus();
  }

  // LED indication
  if (!wifiConnected && WiFi.getMode() == WIFI_AP) {
    // Blink slowly for AP mode
    if (now - lastWiFiStatusCheck > 250) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  } else if (wifiConnected) {
    digitalWrite(LED_PIN, HIGH); // Solid ON for connected
  }

  // LED status indication
  handleLEDStatus();

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
