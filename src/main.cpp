/**
 * BYD Dolphin Mini — iCar BLE OBD2 Bridge
 *
 * Reads vehicle data via BLE from an iCar/Vgate ELM327 OBD2 adapter
 * and outputs to Serial every 5 seconds.
 *
 * Data sources confirmed via Car Scanner app log analysis:
 *
 *   ECU 781 (BMS — response on 789):
 *     PID 0005: Battery SOC (%) — 1 byte, direct percentage
 *     PID 0008: Battery Voltage  — 2 bytes LE
 *     PID 0009: Battery Current  — 2 bytes LE, (val-5000)/10 = A
 *     PID 1FFD: Pack data        — 4 bytes (details TBD)
 *     PID 000A: Unknown          — 2 bytes LE
 *     PID 000E: Unknown          — 2 bytes LE
 *     PID 000B: Unknown          — 2 bytes LE (91 observed)
 *     PID 0004: Unknown          — 2 bytes LE (62 observed)
 *     PID 000F: Unknown          — 3 bytes LE
 *     PID 0010: Unknown          — 3 bytes LE
 *
 *   ECU 743 (VCU — response on 74B, mirror of 7E0/7E8):
 *     PID 0026: Odometer         — 3 bytes LE, /10 = km
 *     PID 0104: Battery capacity — 2 bytes LE, /100 = Ah (50.00 observed)
 *     PID 0105: Battery capacity — same as 0104
 *     PID 001F: Uptime counter   — NOT SOC (increments over time)
 *
 *   Broadcast 7DF:
 *     Service 09 PID 02: VIN (17 chars, multi-frame)
 *     AT RV: 12V battery voltage
 *
 * Protocol: CAN 500kbps 11-bit (ELM327 protocol 6)
 * Tested with: Vgate iCar Pro BLE 4.0 (service UUID 18F0)
 */

#include <Arduino.h>
#include <BLEDevice.h>

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

static const int MAX_RESPONSE_LEN = 1024;
static const uint32_t POLL_INTERVAL_MS = 5000;

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
static bool elmReady = false;

// Vehicle data
static char  vin[18]    = {0};
static int   soc        = -1;    // % (from ECU 781)
static float odometer   = -1;    // km (from ECU 743)
static float batteryV   = -1;    // V (from ECU 781, PID 0008)
static float currentA   = -1;    // A (from ECU 781, PID 0009)
static float auxBattV   = -1;    // V (from ELM327 AT RV)
static float capacity   = -1;    // Ah (from ECU 743, PID 0104)
static bool  carAwake   = false;

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
  void onDisconnect(BLEClient*) override {
    connected = false;
    elmReady = false;
    Serial.println("[BLE] Disconnected");
  }
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

// Find data bytes after "62PPPP" in a UDS positive response (ATS0 = no spaces)
// Response format: "78904620005XX" or "789046200054E"
// We find "62", skip the 2-byte PID echo (4 hex chars), then data starts.
static const char* findDataStart(const char* resp) {
  const char* p = strstr(resp, "62");
  if (!p) return NULL;
  p += 6;  // skip "62PPPP" (service byte "62" + 4 hex PID chars, no spaces)
  return isxdigit(*p) ? p : NULL;
}

// Parse a 2-byte little-endian value from compact hex (no spaces)
// e.g. "2A01" -> b0=0x2A, b1=0x01 -> LE = 0x012A = 298
static int parseLE16(const char* dataStart) {
  int b0 = parseHexByte(dataStart);
  int b1 = parseHexByte(dataStart + 2);
  if (b0 < 0 || b1 < 0) return -1;
  return b0 | (b1 << 8);
}

// Parse a 3-byte little-endian value from compact hex
// e.g. "642C01" -> 0x012C64 = 76900
static int parseLE24(const char* dataStart) {
  int b0 = parseHexByte(dataStart);
  int b1 = parseHexByte(dataStart + 2);
  int b2 = parseHexByte(dataStart + 4);
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

  Serial.printf("[BLE] Connected via %s\n", pActiveProfile->name);
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
  Serial.println("[ELM] Ready");
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
  // Response (ATS0, ATH1): 7E810144902014C47587E82143453443433053 7E82230303437323130
  // CAN ID "7E8" is 3 hex chars (odd), so byte pairs misalign across frames.
  // Parse frame by frame: find "7E8", extract data bytes after it.
  //   Frame 1: 7E8 [10 14] [49 02 01] [4C 47 58]     (first frame + svc header + 3 VIN bytes)
  //   Frame 2: 7E8 [21] [43 45 34 43 43 30 53]        (continuation + 7 VIN bytes)
  //   Frame 3: 7E8 [22] [30 30 34 37 32 31 30]        (continuation + 7 VIN bytes)
  int vinIdx = 0;
  const char* p = responseBuf;
  int frameNum = 0;

  while (*p && vinIdx < 17) {
    // Find next "7E8" frame header
    const char* frame = strstr(p, "7E8");
    if (!frame) break;
    const char* data = frame + 3;  // skip "7E8" (3 hex chars)

    // Find end of this frame (next "7E8" or end of string)
    const char* nextFrame = strstr(data, "7E8");
    int frameLen = nextFrame ? (nextFrame - data) : strlen(data);

    if (frameNum == 0) {
      // First frame: skip "1014490201" (FF header + length + service response)
      // = 10 chars: "10" + "14" + "490201"
      if (frameLen >= 10) {
        const char* vinData = data + 10;
        while (vinData < data + frameLen && vinIdx < 17) {
          int b = parseHexByte(vinData);
          if (b >= 0x20 && b <= 0x7E) vin[vinIdx++] = (char)b;
          vinData += 2;
        }
      }
    } else {
      // Continuation frame: skip "2X" (2 hex chars for seq number)
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
    p = data;  // advance past this frame header
  }
}

// ─── Data Reading ───────────────────────────────────────────────────────────

static void readBMSData() {
  // ECU 781 — BMS (response on 789)
  setHeader("781");

  // SOC (PID 0005) — 1 byte, direct percentage
  if (sendCmd("220005") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) {
      int val = parseHexByte(d);
      if (val >= 0) {
        soc = val;
        carAwake = true;
      }
    }
  }
  delay(60);

  if (!carAwake) return;

  // Voltage (PID 0008) — 2 bytes LE
  if (sendCmd("220008") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) {
      int raw = parseLE16(d);
      if (raw >= 0) batteryV = raw / 10.0f;
    }
  }
  delay(60);

  // Current (PID 0009) — 2 bytes LE, (val-5000)/10 = A
  if (sendCmd("220009") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) {
      int raw = parseLE16(d);
      if (raw >= 0) currentA = (raw - 5000) / 10.0f;
    }
  }
  delay(60);
}

static void readVCUData() {
  // ECU 743 — VCU (response on 74B, mirror of 7E0/7E8)
  setHeader("743");

  // Odometer (PID 0026) — 3 bytes LE, /10 = km
  if (sendCmd("220026") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) {
      int raw = parseLE24(d);
      if (raw >= 0) odometer = raw / 10.0f;
    }
  }
  delay(60);

  // Capacity (PID 0104) — 2 bytes LE, /100 = Ah
  if (carAwake && sendCmd("220104") && !hasError()) {
    const char* d = findDataStart(responseBuf);
    if (d) {
      int raw = parseLE16(d);
      if (raw > 0) capacity = raw / 100.0f;
    }
  }
  delay(60);
}

static void read12VBattery() {
  // 12V battery via ELM327 internal voltage reading
  sendCmd("ATSH7DF"); delay(30);
  if (sendCmd("ATRV")) {
    float v = atof(responseBuf);
    if (v > 0) auxBattV = v;
  }
}

// ─── Display ────────────────────────────────────────────────────────────────

static void printDashboard() {
  Serial.println("\n========================================");
  Serial.println("   BYD Dolphin Mini - Vehicle Data");
  Serial.println("========================================");

  if (vin[0])       Serial.printf("  VIN:              %s\n", vin);
  if (soc >= 0)     Serial.printf("  Battery SOC:      %d%%\n", soc);
  if (odometer >= 0) Serial.printf("  Odometer:         %.1f km\n", odometer);
  if (batteryV >= 0) Serial.printf("  Battery Voltage:  %.1f V\n", batteryV);
  if (currentA > -499) Serial.printf("  Battery Current:  %.1f A\n", currentA);
  if (capacity >= 0) Serial.printf("  Battery Capacity: %.2f Ah\n", capacity);
  if (auxBattV >= 0) Serial.printf("  12V Battery:      %.1f V\n", auxBattV);

  Serial.printf("  Car State:        %s\n", carAwake ? "READY" : "sleeping");

  Serial.println("========================================\n");
}

// ─── Main ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== iCar BLE Bridge for BYD Dolphin Mini ===\n");
  BLEDevice::init("BYD-Bridge");

  for (int i = 0; i < 3; i++) {
    if (bleConnect()) break;
    Serial.printf("[BLE] Retry %d...\n", i + 1);
    delay(3000);
  }
  if (!connected) return;
  delay(500);
  if (!initElm()) return;
  elmReady = true;

  readVIN();
  readBMSData();
  readVCUData();
  read12VBattery();
  printDashboard();
}

void loop() {
  if (!elmReady || !connected) {
    Serial.println("[!] Reconnecting...");
    delay(3000);
    if (bleConnect() && initElm()) {
      elmReady = true;
      readVIN();
    }
    return;
  }

  delay(POLL_INTERVAL_MS);
  readBMSData();
  readVCUData();
  read12VBattery();
  printDashboard();
}
