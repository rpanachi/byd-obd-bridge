#pragma once

#include <cstdio>
#include <ctime>
#include <cstring>
#include "obd_parser.h"

inline int formatISO8601(char* buf, int bufLen, time_t t, long gmtOffsetSec = 0) {
  time_t local = t + gmtOffsetSec;
  struct tm tmLocal;
  gmtime_r(&local, &tmLocal);

  int len = strftime(buf, bufLen, "%Y-%m-%dT%H:%M:%S", &tmLocal);
  if (gmtOffsetSec == 0) {
    len += snprintf(buf + len, bufLen - len, "Z");
  } else {
    int totalMin = (int)(gmtOffsetSec / 60);
    char sign = totalMin < 0 ? '-' : '+';
    if (totalMin < 0) totalMin = -totalMin;
    len += snprintf(buf + len, bufLen - len, "%c%02d:%02d", sign, totalMin / 60, totalMin % 60);
  }
  return len;
}

inline int buildPayloadJSON(char* buf, int bufLen, const VehicleData& v, const char* isoTime) {
  return snprintf(buf, bufLen,
    "{\"vin\":\"%s\",\"timestamp\":\"%s\",\"odometer\":%.1f,"
    "\"battery_soc\":%d,\"battery_v\":%.1f,\"current_a\":%.1f}",
    v.vin, isoTime, v.odometer, (int)v.soc, v.batteryV, v.currentA);
}
