#pragma once

#include <cstdio>
#include <ctime>
#include "json_builder.h"

// ─── Time Validation ────────────────────────────────────────────────────────

// Minimum valid timestamp: 2025-01-01T00:00:00Z
static const time_t MIN_VALID_EPOCH = 1735689600;

inline bool isTimeValid(time_t t) {
  return t >= MIN_VALID_EPOCH;
}

// ─── HTTP Response Validation ───────────────────────────────────────────────

inline bool isHTTPSuccess(int code) {
  return code >= 200 && code < 500;
}

// ─── Payload Preparation ────────────────────────────────────────────────────

inline int preparePayload(char* json, int jsonLen, const VehicleData& v, time_t now, long gmtOffsetSec = 0) {
  char isoTime[30];
  formatISO8601(isoTime, sizeof(isoTime), now, gmtOffsetSec);
  return buildPayloadJSON(json, jsonLen, v, isoTime);
}
