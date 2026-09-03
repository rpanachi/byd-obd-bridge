#pragma once

#include <cmath>
#include "obd_parser.h"

// ─── Plausibility Limits ────────────────────────────────────────────────────
//
// A reading is only posted when every value below is inside its band. The
// bands are deliberately wide: they exist to reject readings taken before the
// car is fully READY (ECUs report 0 until they have synced) and values parsed
// from the wrong bytes, not to second-guess the car.

static const float SOC_MIN_PCT       = 1.0f;       // 0 = BMS not initialised
static const float SOC_MAX_PCT       = 100.0f;
static const float ODOMETER_MIN_KM   = 0.0f;       // exclusive: 0 = VCU not synced
static const float ODOMETER_MAX_KM   = 1000000.0f; // exclusive: padding bytes parsed as data
static const float BATTERY_V_MIN     = 20.0f;      // pack voltage in the /10 scale of PID 0008
static const float BATTERY_V_MAX     = 40.0f;

// ─── Agreement Between Consecutive Reads ────────────────────────────────────
//
// Two reads taken about a second apart must agree on the values that can't
// legitimately change in that time. Voltage and current move with load and
// are not compared.

static const float ODOMETER_AGREE_KM = 0.2f;
static const float SOC_AGREE_PCT     = 1.0f;

inline bool isSOCValid(float soc) {
  return soc >= SOC_MIN_PCT && soc <= SOC_MAX_PCT;
}

inline bool isOdometerValid(float km) {
  return km > ODOMETER_MIN_KM && km < ODOMETER_MAX_KM;
}

inline bool isBatteryVValid(float v) {
  return v >= BATTERY_V_MIN && v <= BATTERY_V_MAX;
}

inline bool isVehicleDataValid(const VehicleData& v) {
  return isSOCValid(v.soc) && isOdometerValid(v.odometer) && isBatteryVValid(v.batteryV);
}

inline bool vehicleDataAgrees(const VehicleData& a, const VehicleData& b) {
  return std::fabs(a.odometer - b.odometer) <= ODOMETER_AGREE_KM &&
         std::fabs(a.soc - b.soc) <= SOC_AGREE_PCT;
}
