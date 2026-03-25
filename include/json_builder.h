#pragma once

#include <cstdio>
#include <ctime>
#include <cstring>
#include "obd_parser.h"

inline int formatISO8601(char* buf, int bufLen, time_t t) {
  struct tm tmUTC;
  gmtime_r(&t, &tmUTC);
  return strftime(buf, bufLen, "%Y-%m-%dT%H:%M:%SZ", &tmUTC);
}

inline int buildPayloadJSON(char* buf, int bufLen, const VehicleData& v, const char* isoTime) {
  return snprintf(buf, bufLen,
    "{\"vin\":\"%s\",\"timestamp\":\"%s\",\"odometer\":%.1f,"
    "\"battery_soc\":%d,\"battery_v\":%.1f,\"current_a\":%.1f}",
    v.vin, isoTime, v.odometer, (int)v.soc, v.batteryV, v.currentA);
}
