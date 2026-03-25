#pragma once

#include <cstring>
#include <cctype>
#include <cstdlib>
// ─── Vehicle Data ───────────────────────────────────────────────────────────

struct VehicleData {
  char  vin[18];
  float soc;
  float odometer;
  float batteryV;
  float currentA;
  float auxBattV;
  float capacity;
};

inline void vehicleDataInit(VehicleData& v) {
  memset(v.vin, 0, sizeof(v.vin));
  v.soc = v.odometer = v.batteryV = v.currentA = v.auxBattV = v.capacity = -1;
}

// ─── PID Table ──────────────────────────────────────────────────────────────

struct PIDDef {
  const char* header;
  const char* cmd;
  int    bytes;
  float  divisor;
  int    offset;
  int    targetOffset;  // offsetof into VehicleData
};

#define PID_TARGET(field) ((int)offsetof(VehicleData, field))

inline float* pidTarget(VehicleData& v, const PIDDef& pid) {
  return (float*)((char*)&v + pid.targetOffset);
}

static const PIDDef PID_TABLE[] = {
  // BMS (header 781)
  {"781", "220005", 1,   1.0f,     0, PID_TARGET(soc)},       // SOC (%)
  {"781", "220008", 2,  10.0f,     0, PID_TARGET(batteryV)},   // Battery Voltage
  {"781", "220009", 2,  10.0f, -5000, PID_TARGET(currentA)},   // Battery Current
  // VCU (header 743)
  {"743", "220026", 3,  10.0f,     0, PID_TARGET(odometer)},   // Odometer (km)
  {"743", "220104", 2, 100.0f,     0, PID_TARGET(capacity)},   // Capacity (Ah)
};
static const int PID_COUNT = sizeof(PID_TABLE) / sizeof(PID_TABLE[0]);

// ─── Hex Parsing ────────────────────────────────────────────────────────────

inline int parseHexByte(const char* p) {
  if (!p || !isxdigit(p[0]) || !isxdigit(p[1])) return -1;
  char hex[3] = {p[0], p[1], 0};
  return (int)strtol(hex, NULL, 16);
}

inline int parseLittleEndian(const char* d, int bytes) {
  int result = 0;
  for (int i = 0; i < bytes; i++) {
    int b = parseHexByte(d + i * 2);
    if (b < 0) return -1;
    result |= (b << (i * 8));
  }
  return result;
}

inline const char* findUDSData(const char* resp) {
  const char* p = strstr(resp, "62");
  if (!p) return NULL;
  p += 6;
  return isxdigit(*p) ? p : NULL;
}

// ─── PID Response Processing ────────────────────────────────────────────────

inline bool applyPIDResponse(VehicleData& v, const PIDDef& pid, const char* response) {
  const char* d = findUDSData(response);
  if (!d) return false;
  int raw = parseLittleEndian(d, pid.bytes);
  if (raw < 0) return false;
  *pidTarget(v, pid) = (raw + pid.offset) / pid.divisor;
  return true;
}

// ─── VIN Parsing ────────────────────────────────────────────────────────────

inline int parseVIN(const char* response, char* vin, int maxLen) {
  memset(vin, 0, maxLen);
  int vinIdx = 0;
  const char* p = response;
  int frameNum = 0;

  while (*p && vinIdx < 17 && vinIdx < maxLen - 1) {
    const char* frame = strstr(p, "7E8");
    if (!frame) break;
    const char* data = frame + 3;
    const char* nextFrame = strstr(data, "7E8");
    int frameLen = nextFrame ? (nextFrame - data) : strlen(data);
    int skip = (frameNum == 0) ? 10 : 2;

    if (frameLen >= skip) {
      const char* vinData = data + skip;
      while (vinData < data + frameLen && vinIdx < 17 && vinIdx < maxLen - 1) {
        int b = parseHexByte(vinData);
        if (b >= 0x20 && b <= 0x7E) vin[vinIdx++] = (char)b;
        vinData += 2;
      }
    }
    frameNum++;
    p = data;
  }
  return vinIdx;
}
