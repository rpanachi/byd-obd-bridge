#include <unity.h>
#include "json_builder.h"

void setUp(void) {}
void tearDown(void) {}

// ─── buildPayloadJSON ───────────────────────────────────────────────────────

void test_buildPayloadJSON_format(void) {
  VehicleData v;
  vehicleDataInit(v);
  strcpy(v.vin, "LGXCE4CB1R0123456");
  v.soc = 65;
  v.odometer = 7703.3f;
  v.batteryV = 29.8f;
  v.currentA = 2.3f;

  char json[512];
  buildPayloadJSON(json, sizeof(json), v, "2026-03-25T12:30:00Z");

  TEST_ASSERT_EQUAL_STRING(
    "{\"vin\":\"LGXCE4CB1R0123456\",\"timestamp\":\"2026-03-25T12:30:00Z\","
    "\"odometer\":7703.3,\"battery_soc\":65,\"battery_v\":29.8,\"current_a\":2.3}",
    json);
}

void test_buildPayloadJSON_zero_soc(void) {
  VehicleData v;
  vehicleDataInit(v);
  strcpy(v.vin, "VIN123");
  v.soc = 0;
  v.odometer = 100.0f;

  char json[256];
  buildPayloadJSON(json, sizeof(json), v, "2026-01-01T00:00:00Z");

  TEST_ASSERT_NOT_NULL(strstr(json, "\"battery_soc\":0"));
}

void test_buildPayloadJSON_empty_vin(void) {
  VehicleData v;
  vehicleDataInit(v);
  v.soc = 50;
  v.odometer = 500.0f;

  char json[256];
  buildPayloadJSON(json, sizeof(json), v, "2026-01-01T00:00:00Z");

  TEST_ASSERT_NOT_NULL(strstr(json, "\"vin\":\"\""));
}

// ─── formatISO8601 ──────────────────────────────────────────────────────────

void test_formatISO8601(void) {
  char buf[25];
  time_t t = 1767225600;
  formatISO8601(buf, sizeof(buf), t);
  TEST_ASSERT_EQUAL_STRING("2026-01-01T00:00:00Z", buf);
}

void test_formatISO8601_epoch(void) {
  char buf[25];
  time_t t = 0;
  formatISO8601(buf, sizeof(buf), t);
  TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00Z", buf);
}

// ─── Runner ─────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  UNITY_BEGIN();

  RUN_TEST(test_buildPayloadJSON_format);
  RUN_TEST(test_buildPayloadJSON_zero_soc);
  RUN_TEST(test_buildPayloadJSON_empty_vin);
  RUN_TEST(test_formatISO8601);
  RUN_TEST(test_formatISO8601_epoch);

  return UNITY_END();
}
