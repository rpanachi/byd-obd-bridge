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

// ─── Constants ──────────────────────────────────────────────────────────────

static const int MAX_READ_ATTEMPTS = 5;
static const int READ_RETRY_DELAY_MS = 10000;
static const int MAX_POST_ATTEMPTS = 5;
static const int POST_RETRY_DELAY_MS = 3000;
static const int WIFI_TIMEOUT_MS = 30000;
static const int MAX_WIFI_ATTEMPTS = 3;
static const int MAX_RESPONSE_LEN = 1024;

// ─── RTC Memory (survives restart, lost on deep sleep) ──────────────────────

enum BootPhase { BOOT_BLE = 0, BOOT_WIFI = 1 };

RTC_NOINIT_ATTR int   rtc_phase;
RTC_NOINIT_ATTR int   rtc_soc;
RTC_NOINIT_ATTR float rtc_odometer;
RTC_NOINIT_ATTR char  rtc_vin[18];
RTC_NOINIT_ATTR uint32_t rtc_magic;  // validates RTC data

static const uint32_t RTC_MAGIC = 0xB1D0DA7A;

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

// ─── Globals ────────────────────────────────────────────────────────────────

static BLERemoteCharacteristic* pTxChar = nullptr;
static BLERemoteCharacteristic* pRxChar = nullptr;
static BLEClient* pClient = nullptr;
static BLEAdvertisedDevice* pTargetDevice = nullptr;
static const BLEProfile* pActiveProfile = nullptr;

static char responseBuf[MAX_RESPONSE_LEN];
static volatile int responseLen = 0;
static volatile bool responseReady = false;
static bool deviceFound = false;
static bool connected = false;

// Vehicle data
static char  vin[18]    = {0};
static int   soc        = -1;
static float odometer   = -1;
static float batteryV   = -1;
static float currentA   = -1;
static float auxBattV   = -1;
static float capacity   = -1;

// ─── Deep Sleep ─────────────────────────────────────────────────────────────

static void enterDeepSleep() {
  Serial.println("[SLEEP] Entering deep sleep...");
  Serial.flush();
  delay(100);
  esp_deep_sleep_start();
}

// ─── BLE Callbacks ──────────────────────────────────────────────────────────

static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData,
                            size_t length, bool isNotify) {
  for (size_t i = 0; i < length && responseLen < MAX_RESPONSE_LEN - 1; i++) {
    char c = (char)pData[i];
    if (c == '>') {
      responseBuf[responseLen] = '\0';
      responseReady = true;
      return;
    }
    responseBuf[responseLen++] = c;
  }
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    for (int i = 0; i < PROFILE_COUNT; i++) {
      if (dev.haveServiceUUID() && dev.isAdvertisingService(PROFILES[i].service)) {
        pTargetDevice = new BLEAdvertisedDevice(dev);
        pActiveProfile = &PROFILES[i];
        deviceFound = true;
        dev.getScan()->stop();
        return;
      }
    }
    String name = dev.getName().c_str();
    name.toUpperCase();
    if (name.indexOf("ICAR") >= 0 || name.indexOf("VGATE") >= 0 ||
        name.indexOf("OBD") >= 0 || name.indexOf("ELM") >= 0) {
      pTargetDevice = new BLEAdvertisedDevice(dev);
      pActiveProfile = &PROFILES[0];
      deviceFound = true;
      dev.getScan()->stop();
    }
  }
};

class ClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient*) override { connected = true; }
  void onDisconnect(BLEClient*) override { connected = false; }
};

// ─── ELM327 Communication ──────────────────────────────────────────────────

static bool sendCmd(const char* cmd, uint32_t timeout = 4000) {
  responseLen = 0;
  responseReady = false;
  memset(responseBuf, 0, sizeof(responseBuf));
  String data = String(cmd) + "\r";
  pTxChar->writeValue((uint8_t*)data.c_str(), data.length());
  uint32_t start = millis();
  while (!responseReady && (millis() - start) < timeout) delay(20);
  return responseReady;
}

static bool hasError() {
  return strstr(responseBuf, "NO DATA") || strstr(responseBuf, "ERROR") ||
         strstr(responseBuf, "UNABLE") || strstr(responseBuf, "?");
}

static int parseHexByte(const char* p) {
  if (!p || !isxdigit(p[0]) || !isxdigit(p[1])) return -1;
  char hex[3] = {p[0], p[1], 0};
  return (int)strtol(hex, NULL, 16);
}

static const char* findDataStart(const char* resp) {
  const char* p = strstr(resp, "62");
  if (!p) return NULL;
  p += 6;
  return isxdigit(*p) ? p : NULL;
}

static int parseLE16(const char* d) {
  int b0 = parseHexByte(d);
  int b1 = parseHexByte(d + 2);
  if (b0 < 0 || b1 < 0) return -1;
  return b0 | (b1 << 8);
}

static int parseLE24(const char* d) {
  int b0 = parseHexByte(d);
  int b1 = parseHexByte(d + 2);
  int b2 = parseHexByte(d + 4);
  if (b0 < 0 || b1 < 0 || b2 < 0) return -1;
  return b0 | (b1 << 8) | (b2 << 16);
}

// ─── BLE Connection ─────────────────────────────────────────────────────────

static bool bleConnect() {
  Serial.println("[BLE] Scanning for iCar adapter...");
  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  deviceFound = false;
  pScan->start(15, false);
  pScan->clearResults();
  if (!deviceFound) {
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
  if (pRxChar->canNotify()) pRxChar->registerForNotify(notifyCallback);

  return true;
}

static bool initElm() {
  const char* cmds[] = {
    "ATZ", "ATE0", "ATL0", "ATS0", "ATH1",
    "ATSP6", "ATM0", "ATAT1", "ATAL", "ATST64"
  };
  for (auto c : cmds) {
    if (!sendCmd(c, (strcmp(c, "ATZ") == 0) ? 5000 : 3000)) return false;
    delay(100);
  }
  return true;
}

static void setHeader(const char* hdr) {
  char cmd[20];
  snprintf(cmd, sizeof(cmd), "ATSH%s", hdr);
  sendCmd(cmd); delay(30);
}

// ─── VIN Reading ────────────────────────────────────────────────────────────

static void readVIN() {
  sendCmd("ATSH7DF"); delay(30);
  if (!sendCmd("0902", 5000) || hasError()) return;

  memset(vin, 0, sizeof(vin));
  int vinIdx = 0;
  const char* p = responseBuf;
  int frameNum = 0;

  while (*p && vinIdx < 17) {
    const char* frame = strstr(p, "7E8");
    if (!frame) break;
    const char* data = frame + 3;
    const char* nextFrame = strstr(data, "7E8");
    int frameLen = nextFrame ? (nextFrame - data) : strlen(data);

    if (frameNum == 0) {
      if (frameLen >= 10) {
        const char* vinData = data + 10;
        while (vinData < data + frameLen && vinIdx < 17) {
          int b = parseHexByte(vinData);
          if (b >= 0x20 && b <= 0x7E) vin[vinIdx++] = (char)b;
          vinData += 2;
        }
      }
    } else {
      if (frameLen >= 2) {
        const char* vinData = data + 2;
        while (vinData < data + frameLen && vinIdx < 17) {
          int b = parseHexByte(vinData);
          if (b >= 0x20 && b <= 0x7E) vin[vinIdx++] = (char)b;
          vinData += 2;
        }
      }
    }
    frameNum++;
    p = data;
  }
}

// ─── OBD Data Reading ───────────────────────────────────────────────────────

static void readAllData() {
  setHeader("781");

  if (sendCmd("220005") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) { int val = parseHexByte(d); if (val >= 0) soc = val; }
  }
  delay(60);

  if (sendCmd("220008") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) { int raw = parseLE16(d); if (raw >= 0) batteryV = raw / 10.0f; }
  }
  delay(60);

  if (sendCmd("220009") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) { int raw = parseLE16(d); if (raw >= 0) currentA = (raw - 5000) / 10.0f; }
  }
  delay(60);

  setHeader("743");

  if (sendCmd("220026") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) { int raw = parseLE24(d); if (raw >= 0) odometer = raw / 10.0f; }
  }
  delay(60);

  if (sendCmd("220104") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) { int raw = parseLE16(d); if (raw > 0) capacity = raw / 100.0f; }
  }
  delay(60);

  sendCmd("ATSH7DF"); delay(30);
  if (sendCmd("ATRV")) {
    float v = atof(responseBuf);
    if (v > 0) auxBattV = v;
  }

  if (!vin[0]) readVIN();
}

// ─── Display ────────────────────────────────────────────────────────────────

static void printDashboard() {
  Serial.println("\n========================================");
  Serial.println("   BYD Dolphin Mini - Vehicle Data");
  Serial.println("========================================");
  if (vin[0])        Serial.printf("  VIN:              %s\n", vin);
  if (soc >= 0)      Serial.printf("  Battery SOC:      %d%%\n", soc);
  if (odometer >= 0) Serial.printf("  Odometer:         %.1f km\n", odometer);
  if (batteryV >= 0) Serial.printf("  Battery Voltage:  %.1f V\n", batteryV);
  if (currentA > -499) Serial.printf("  Battery Current:  %.1f A\n", currentA);
  if (capacity >= 0) Serial.printf("  Battery Capacity: %.2f Ah\n", capacity);
  if (auxBattV >= 0) Serial.printf("  12V Battery:      %.1f V\n", auxBattV);
  Serial.println("========================================\n");
}

// ─── Phase 1: BLE Read ─────────────────────────────────────────────────────

static void phaseBLE() {
  Serial.println("[1/5] Connecting to iCar adapter...");
  BLEDevice::init("BYD-Bridge");
  for (int i = 0; i < 3; i++) {
    if (bleConnect()) break;
    Serial.printf("[1/5] Retry %d...\n", i + 1);
    delay(3000);
  }
  if (!connected) {
    Serial.println("[1/5] FAILED — iCar not found, aborting");
    enterDeepSleep();
  }
  Serial.println("[1/5] Connected to iCar Pro");
  if (!initElm()) {
    Serial.println("[1/5] FAILED — ELM327 init error, aborting");
    enterDeepSleep();
  }

  Serial.println("[2/5] Reading vehicle data...");
  bool dataValid = false;
  for (int attempt = 1; attempt <= MAX_READ_ATTEMPTS; attempt++) {
    Serial.printf("[2/5] Read attempt %d/%d...\n", attempt, MAX_READ_ATTEMPTS);
    readAllData();

    if (soc >= 0 && odometer >= 0) {
      dataValid = true;
      Serial.printf("[2/5] Read successful — SOC=%d%% Odometer=%.1f km\n", soc, odometer);
      break;
    }

    if (attempt < MAX_READ_ATTEMPTS) {
      Serial.printf("[2/5] No valid data, retrying in %ds...\n", READ_RETRY_DELAY_MS / 1000);
      delay(READ_RETRY_DELAY_MS);
    }
  }

  printDashboard();

  Serial.println("[3/5] Disconnecting from iCar...");
  if (pClient && connected) pClient->disconnect();
  delay(200);
  Serial.println("[3/5] Disconnected from iCar");

  if (!dataValid) {
    Serial.println("[3/5] FAILED — no valid car data, aborting");
    enterDeepSleep();
  }

  // Save to RTC memory and restart into WiFi phase
  rtc_soc = soc;
  rtc_odometer = odometer;
  strncpy(rtc_vin, vin, sizeof(rtc_vin));
  rtc_magic = RTC_MAGIC;
  rtc_phase = BOOT_WIFI;

  Serial.println("[3/5] Restarting for WiFi phase...");
  Serial.flush();
  delay(100);
  ESP.restart();
}

// ─── Phase 2: WiFi POST ────────────────────────────────────────────────────

static void phaseWiFi() {
  // Restore data from RTC memory
  soc = rtc_soc;
  odometer = rtc_odometer;
  strncpy(vin, rtc_vin, sizeof(vin));
  Serial.printf("[4/5] Data from OBD: SOC=%d%% Odometer=%.1f km VIN=%s\n", soc, odometer, vin);

  // Clear RTC phase so next cold boot starts with BLE
  rtc_phase = BOOT_BLE;

  Serial.println("[4/5] Connecting to WiFi...");
  WiFi.mode(WIFI_STA);

  bool wifiConnected = false;
  for (int attempt = 1; attempt <= MAX_WIFI_ATTEMPTS; attempt++) {
    Serial.printf("[WiFi] Attempt %d/%d...\n", attempt, MAX_WIFI_ATTEMPTS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
      delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      break;
    }

    WiFi.disconnect(true);
    delay(1000);
  }

  if (!wifiConnected) {
    Serial.println("[4/5] FAILED — WiFi connection error, aborting");
    enterDeepSleep();
  }

  Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.println("[4/5] Connected to WiFi");

  // Sync time via NTP
  configTime(0, 0, "pool.ntp.org");
  struct tm timeinfo;
  getLocalTime(&timeinfo, 5000);

  // POST
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(POST_URL);
  http.addHeader("Content-Type", "application/json");

  time_t now;
  time(&now);
  struct tm tmNow;
  gmtime_r(&now, &tmNow);
  char isoTime[25];
  strftime(isoTime, sizeof(isoTime), "%Y-%m-%dT%H:%M:%SZ", &tmNow);

  char json[256];
  snprintf(json, sizeof(json),
    "{\"vin\":\"%s\",\"timestamp\":\"%s\",\"odometer\":%.1f,\"battery\":%d}",
    vin, isoTime, odometer, soc);

  bool postSuccess = false;
  for (int attempt = 1; attempt <= MAX_POST_ATTEMPTS; attempt++) {
    Serial.printf("[4/5] POST attempt %d/%d...\n", attempt, MAX_POST_ATTEMPTS);
    Serial.printf("[HTTP] POST %s\n", POST_URL);
    Serial.printf("[HTTP] Payload: %s\n", json);

    int httpCode = http.POST(json);
    Serial.printf("[HTTP] Response: %d\n", httpCode);

    if (httpCode >= 200 && httpCode < 500) {
      postSuccess = true;
      Serial.println("[4/5] Data sent successfully");
      break;
    }
    if (attempt < MAX_POST_ATTEMPTS) delay(POST_RETRY_DELAY_MS);
  }

  http.end();

  if (!postSuccess) {
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

  if (rtc_magic == RTC_MAGIC && rtc_phase == BOOT_WIFI) {
    Serial.println("[BOOT] Phase 2 — WiFi POST");
    phaseWiFi();
  } else {
    Serial.println("[BOOT] Phase 1 — BLE Read");
    phaseBLE();
  }
}

void loop() {
  // Never reached — both phases end with deep sleep or restart
}
