/**
 * BYD Dolphin Mini — iCar BLE OBD2 Bridge
 *
 * Two-phase operation (BLE and WiFi can't share the radio reliably):
 *   Phase 1 (BOOT_BLE): Connect BLE, read OBD data, save to RTC memory, restart
 *   Phase 2 (BOOT_WIFI): Connect WiFi, POST JSON, deep sleep
 *
 * Data sources (CAN 500kbps 11-bit, ELM327 protocol 6):
 *   ECU 781 (BMS):  SOC (PID 0005), Voltage (0008), Current (0009)
 *   ECU 743 (VCU):  Odometer (PID 0026), Capacity (0104)
 *   Broadcast 7DF:  VIN (service 09 PID 02), 12V battery (AT RV)
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"
#include "obd_parser.h"
#include "http_post.h"

// ─── Configuration ──────────────────────────────────────────────────────────

static const int BLE_CONNECT_ATTEMPTS     = 5;
static const int BLE_CONNECT_DELAY_MS     = 5000;
static const int OBD_READ_ATTEMPTS        = 5;
static const int OBD_READ_DELAY_MS        = 10000;
static const int WIFI_CONNECT_ATTEMPTS    = 3;
static const int WIFI_CONNECT_TIMEOUT_MS  = 30000;
static const int HTTP_POST_ATTEMPTS       = 5;
static const int HTTP_POST_DELAY_MS       = 3000;
static const int NTP_SYNC_ATTEMPTS        = 3;
static const int NTP_SYNC_TIMEOUT_MS      = 5000;
static const long GMT_OFFSET_SEC          = -3 * 3600; // BRT (UTC-3)
static const int ELM_RESPONSE_MAX         = 1024;

// ─── RTC Memory (survives restart, lost on deep sleep) ──────────────────────

enum BootPhase { BOOT_BLE = 0, BOOT_WIFI = 1 };

static const uint32_t RTC_MAGIC = 0xB1D0DA7A;

RTC_NOINIT_ATTR int      rtc_phase;
RTC_NOINIT_ATTR int      rtc_soc;
RTC_NOINIT_ATTR float    rtc_odometer;
RTC_NOINIT_ATTR float    rtc_batteryV;
RTC_NOINIT_ATTR float    rtc_currentA;
RTC_NOINIT_ATTR char     rtc_vin[18];
RTC_NOINIT_ATTR uint32_t rtc_magic;

// ─── Vehicle Data ───────────────────────────────────────────────────────────

static VehicleData vehicle;

// ─── BLE Profiles ───────────────────────────────────────────────────────────

struct BLEProfile {
  const char* name;
  BLEUUID service;
  BLEUUID rxChar;
  BLEUUID txChar;
};

static const BLEProfile PROFILES[] = {
  { "iCar Pro",
    BLEUUID("000018F0-0000-1000-8000-00805F9B34FB"),
    BLEUUID("00002AF0-0000-1000-8000-00805F9B34FB"),
    BLEUUID("00002AF1-0000-1000-8000-00805F9B34FB") },
  { "ELM327 Clone",
    BLEUUID("0000FFE0-0000-1000-8000-00805F9B34FB"),
    BLEUUID("0000FFE1-0000-1000-8000-00805F9B34FB"),
    BLEUUID("0000FFE1-0000-1000-8000-00805F9B34FB") },
};
static const int PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);

// ─── BLE State ──────────────────────────────────────────────────────────────

static BLERemoteCharacteristic* pTxChar = nullptr;
static BLERemoteCharacteristic* pRxChar = nullptr;
static BLEClient* pClient = nullptr;
static BLEAdvertisedDevice* pTargetDevice = nullptr;
static const BLEProfile* pActiveProfile = nullptr;

static char          elmResponse[ELM_RESPONSE_MAX];
static volatile int  elmResponseLen   = 0;
static volatile bool elmResponseReady = false;
static bool bleDeviceFound = false;
static bool bleConnected   = false;

// ─── Utility ────────────────────────────────────────────────────────────────

static void enterDeepSleep() {
  Serial.println("[SLEEP] Entering deep sleep...");
  Serial.flush();
  delay(100);
  esp_deep_sleep_start();
}

// ─── BLE Callbacks ──────────────────────────────────────────────────────────

static void onElmNotify(BLERemoteCharacteristic*, uint8_t* data,
                         size_t len, bool) {
  for (size_t i = 0; i < len && elmResponseLen < ELM_RESPONSE_MAX - 1; i++) {
    char c = (char)data[i];
    if (c == '>') {
      elmResponse[elmResponseLen] = '\0';
      elmResponseReady = true;
      return;
    }
    elmResponse[elmResponseLen++] = c;
  }
}

static void selectDevice(BLEAdvertisedDevice& dev, const BLEProfile* profile) {
  pTargetDevice = new BLEAdvertisedDevice(dev);
  pActiveProfile = profile;
  bleDeviceFound = true;
  dev.getScan()->stop();
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    for (int i = 0; i < PROFILE_COUNT; i++) {
      if (dev.haveServiceUUID() && dev.isAdvertisingService(PROFILES[i].service)) {
        selectDevice(dev, &PROFILES[i]);
        return;
      }
    }
    String name = dev.getName().c_str();
    name.toUpperCase();
    if (name.indexOf("ICAR") >= 0 || name.indexOf("VGATE") >= 0 ||
        name.indexOf("OBD") >= 0 || name.indexOf("ELM") >= 0) {
      selectDevice(dev, &PROFILES[0]);
    }
  }
};

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override    { bleConnected = true; }
  void onDisconnect(BLEClient*) override { bleConnected = false; }
};

// ─── ELM327 Communication ──────────────────────────────────────────────────

static bool elmSend(const char* cmd, uint32_t timeout = 4000) {
  elmResponseLen = 0;
  elmResponseReady = false;
  memset(elmResponse, 0, sizeof(elmResponse));
  String data = String(cmd) + "\r";
  pTxChar->writeValue((uint8_t*)data.c_str(), data.length());
  uint32_t start = millis();
  while (!elmResponseReady && (millis() - start) < timeout) delay(20);
  return elmResponseReady;
}

static bool elmHasError() {
  return strstr(elmResponse, "NO DATA") || strstr(elmResponse, "ERROR") ||
         strstr(elmResponse, "UNABLE") || strstr(elmResponse, "?");
}

static void elmSetHeader(const char* hdr) {
  char cmd[20];
  snprintf(cmd, sizeof(cmd), "ATSH%s", hdr);
  elmSend(cmd);
  delay(30);
}

static bool elmInit() {
  const char* cmds[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1",
    "ATSP6", "ATM0", "ATAT1", "ATAL", "ATST64"
  };
  for (auto c : cmds) {
    if (!elmSend(c, (strcmp(c, "ATZ") == 0) ? 5000 : 3000)) return false;
    delay(100);
  }
  return true;
}

// ─── BLE Connection ─────────────────────────────────────────────────────────

static bool bleConnect() {
  Serial.println("[BLE] Scanning for iCar adapter...");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  bleDeviceFound = false;
  pScan->start(15, false);
  pScan->clearResults();
  if (!bleDeviceFound) {
    Serial.println("[BLE] iCar adapter not found");
    return false;
  }

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCallbacks());
  if (!pClient->connect(pTargetDevice)) {
    Serial.println("[BLE] Connection failed");
    return false;
  }

  BLERemoteService* svc = pClient->getService(pActiveProfile->service);
  if (!svc) {
    for (int i = 0; i < PROFILE_COUNT; i++) {
      if (&PROFILES[i] != pActiveProfile) {
        svc = pClient->getService(PROFILES[i].service);
        if (svc) { pActiveProfile = &PROFILES[i]; break; }
      }
    }
  }
  if (!svc) return false;

  pRxChar = svc->getCharacteristic(pActiveProfile->rxChar);
  pTxChar = svc->getCharacteristic(pActiveProfile->txChar);
  if (!pRxChar || !pTxChar) return false;
  if (pRxChar->canNotify()) pRxChar->registerForNotify(onElmNotify);

  return true;
}

// ─── READY Mode Detection ───────────────────────────────────────────────────

static bool isVehicleReady() {
  elmSetHeader("781");
  if (!elmSend("220005") || elmHasError()) return false;
  const char* d = findUDSData(elmResponse);
  return d != NULL;
}

// ─── OBD Data Reading ───────────────────────────────────────────────────────

static void readPIDs() {
  const char* currentHeader = NULL;
  for (int i = 0; i < PID_COUNT; i++) {
    const PIDDef& pid = PID_TABLE[i];
    if (!currentHeader || strcmp(currentHeader, pid.header) != 0) {
      elmSetHeader(pid.header);
      currentHeader = pid.header;
    }
    if (elmSend(pid.cmd) && !elmHasError()) {
      applyPIDResponse(vehicle, pid, elmResponse);
    }
    delay(60);
  }
}

static void readAuxBattery() {
  if (elmSend("ATRV")) {
    float v = atof(elmResponse);
    if (v > 0) vehicle.auxBattV = v;
  }
}

static void readVIN() {
  elmSetHeader("7DF");
  if (!elmSend("0902", 5000) || elmHasError()) return;
  parseVIN(elmResponse, vehicle.vin, sizeof(vehicle.vin));
}

static void readAllData() {
  readPIDs();
  readAuxBattery();
  if (!vehicle.vin[0]) readVIN();
}

// ─── Display ────────────────────────────────────────────────────────────────

static void printDashboard() {
  Serial.println("\n========================================");
  Serial.println("   BYD Dolphin Mini - Vehicle Data");
  Serial.println("========================================");
  if (vehicle.vin[0])          Serial.printf("  VIN:              %s\n", vehicle.vin);
  if (vehicle.soc >= 0)        Serial.printf("  Battery SOC:      %d%%\n", (int)vehicle.soc);
  if (vehicle.odometer >= 0)   Serial.printf("  Odometer:         %.1f km\n", vehicle.odometer);
  if (vehicle.batteryV >= 0)   Serial.printf("  Battery Voltage:  %.1f V\n", vehicle.batteryV);
  if (vehicle.currentA > -499) Serial.printf("  Battery Current:  %.1f A\n", vehicle.currentA);
  if (vehicle.capacity >= 0)   Serial.printf("  Battery Capacity: %.2f Ah\n", vehicle.capacity);
  if (vehicle.auxBattV >= 0)   Serial.printf("  12V Battery:      %.1f V\n", vehicle.auxBattV);
  Serial.println("========================================\n");
}

// ─── WiFi ───────────────────────────────────────────────────────────────────

static bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  for (int attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS; attempt++) {
    Serial.printf("[WiFi] Attempt %d/%d...\n", attempt, WIFI_CONNECT_ATTEMPTS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS)
      delay(500);
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect(true);
    delay(1000);
  }
  return false;
}

// ─── HTTP POST ──────────────────────────────────────────────────────────────

static bool syncNTP() {
  for (int attempt = 1; attempt <= NTP_SYNC_ATTEMPTS; attempt++) {
    Serial.printf("[NTP] Sync attempt %d/%d...\n", attempt, NTP_SYNC_ATTEMPTS);
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, NTP_SYNC_TIMEOUT_MS) && isTimeValid(mktime(&timeinfo))) {
      Serial.println("[NTP] Time synced");
      return true;
    }
    Serial.println("[NTP] Sync failed");
  }
  return false;
}

static bool postVehicleData() {
  if (!syncNTP()) {
    Serial.println("[NTP] WARNING — posting with invalid timestamp");
  }

  time_t now;
  time(&now);
  char json[512];
  preparePayload(json, sizeof(json), vehicle, now);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(POST_URL);
  http.addHeader("Content-Type", "application/json");

  bool success = false;
  for (int attempt = 1; attempt <= HTTP_POST_ATTEMPTS; attempt++) {
    Serial.printf("[HTTP] POST attempt %d/%d to %s\n", attempt, HTTP_POST_ATTEMPTS, POST_URL);
    Serial.printf("[HTTP] Payload: %s\n", json);
    int code = http.POST(json);
    Serial.printf("[HTTP] Response: %d\n", code);
    if (isHTTPSuccess(code)) {
      success = true;
      break;
    }
    if (attempt < HTTP_POST_ATTEMPTS) delay(HTTP_POST_DELAY_MS);
  }

  http.end();
  return success;
}

// ─── Phase 1: BLE Read ─────────────────────────────────────────────────────

static void phaseBLE() {
  Serial.println("[1/5] Connecting to iCar adapter...");
  BLEDevice::init("BYD-Bridge");
  for (int i = 0; i < BLE_CONNECT_ATTEMPTS; i++) {
    if (bleConnect()) break;
    Serial.printf("[1/5] Retry %d...\n", i + 1);
    delay(BLE_CONNECT_DELAY_MS);
  }
  if (!bleConnected) {
    Serial.println("[1/5] FAILED — iCar not found, aborting");
    enterDeepSleep();
  }
  Serial.println("[1/5] Connected to iCar Pro");
  if (!elmInit()) {
    Serial.println("[1/5] FAILED — ELM327 init error, aborting");
    enterDeepSleep();
  }

  Serial.println("[2/5] Waiting for vehicle READY mode and reading data...");
  bool dataValid = false;
  for (int attempt = 1; attempt <= OBD_READ_ATTEMPTS; attempt++) {
    Serial.printf("[2/5] Attempt %d/%d...\n", attempt, OBD_READ_ATTEMPTS);

    if (!isVehicleReady()) {
      Serial.printf("[2/5] Vehicle not in READY mode, retrying in %ds...\n", OBD_READ_DELAY_MS / 1000);
      delay(OBD_READ_DELAY_MS);
      continue;
    }
    Serial.println("[2/5] Vehicle is in READY mode, reading data...");

    vehicleDataInit(vehicle);
    readAllData();

    if (vehicle.soc >= 0 && vehicle.odometer >= 0 && vehicle.batteryV >= 0) {
      dataValid = true;
      Serial.printf("[2/5] Read OK — SOC=%d%% Odometer=%.1f km\n",
                    (int)vehicle.soc, vehicle.odometer);
      break;
    }

    Serial.printf("[2/5] Incomplete data, retrying in %ds...\n", OBD_READ_DELAY_MS / 1000);
    if (attempt < OBD_READ_ATTEMPTS) delay(OBD_READ_DELAY_MS);
  }

  printDashboard();

  Serial.println("[3/5] Disconnecting from iCar...");
  if (pClient && bleConnected) pClient->disconnect();
  delay(200);
  Serial.println("[3/5] Disconnected from iCar");

  if (!dataValid) {
    Serial.println("[3/5] FAILED — no valid data, aborting");
    enterDeepSleep();
  }

  rtc_soc = (int)vehicle.soc;
  rtc_odometer = vehicle.odometer;
  rtc_batteryV = vehicle.batteryV;
  rtc_currentA = vehicle.currentA;
  strncpy(rtc_vin, vehicle.vin, sizeof(rtc_vin));
  rtc_magic = RTC_MAGIC;
  rtc_phase = BOOT_WIFI;

  Serial.println("[3/5] Restarting for WiFi phase...");
  Serial.flush();
  delay(100);
  ESP.restart();
}

// ─── Phase 2: WiFi POST ────────────────────────────────────────────────────

static void phaseWiFi() {
  vehicle.soc = rtc_soc;
  vehicle.odometer = rtc_odometer;
  vehicle.batteryV = rtc_batteryV;
  vehicle.currentA = rtc_currentA;
  strncpy(vehicle.vin, rtc_vin, sizeof(vehicle.vin));
  Serial.printf("[4/5] Data from OBD: SOC=%d%% Odometer=%.1f km VIN=%s\n",
                (int)vehicle.soc, vehicle.odometer, vehicle.vin);

  rtc_phase = BOOT_BLE;

  Serial.println("[4/5] Connecting to WiFi...");
  if (!connectWiFi()) {
    Serial.println("[4/5] FAILED — WiFi connection error, aborting");
    enterDeepSleep();
  }
  Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.println("[4/5] Connected to WiFi");

  if (postVehicleData()) {
    Serial.println("[4/5] Data sent successfully");
  } else {
    Serial.println("[4/5] FAILED — POST error after all attempts");
  }

  Serial.println("[5/5] Disconnecting from WiFi...");
  WiFi.disconnect(true);
  Serial.println("[5/5] Disconnected from WiFi");

  enterDeepSleep();
}

// ─── Main ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== iCar BLE Bridge for BYD Dolphin Mini ===\n");

  vehicleDataInit(vehicle);

  if (rtc_magic == RTC_MAGIC && rtc_phase == BOOT_WIFI) {
    Serial.println("[BOOT] Phase 2 — WiFi POST");
    phaseWiFi();
  } else {
    Serial.println("[BOOT] Phase 1 — BLE Read");
    phaseBLE();
  }
}

void loop() {}
