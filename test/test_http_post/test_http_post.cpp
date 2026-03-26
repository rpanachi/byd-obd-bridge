#include <unity.h>
#include "http_post.h"

void setUp(void) {}
void tearDown(void) {}

// ─── isHTTPSuccess ──────────────────────────────────────────────────────────

void test_isHTTPSuccess_2xx(void) {
  TEST_ASSERT_TRUE(isHTTPSuccess(200));
  TEST_ASSERT_TRUE(isHTTPSuccess(201));
  TEST_ASSERT_TRUE(isHTTPSuccess(204));
  TEST_ASSERT_TRUE(isHTTPSuccess(299));
}

void test_isHTTPSuccess_3xx(void) {
  TEST_ASSERT_TRUE(isHTTPSuccess(301));
  TEST_ASSERT_TRUE(isHTTPSuccess(302));
}

void test_isHTTPSuccess_4xx(void) {
  TEST_ASSERT_TRUE(isHTTPSuccess(400));
  TEST_ASSERT_TRUE(isHTTPSuccess(404));
  TEST_ASSERT_TRUE(isHTTPSuccess(422));
  TEST_ASSERT_TRUE(isHTTPSuccess(499));
}

void test_isHTTPSuccess_5xx_fails(void) {
  TEST_ASSERT_FALSE(isHTTPSuccess(500));
  TEST_ASSERT_FALSE(isHTTPSuccess(502));
  TEST_ASSERT_FALSE(isHTTPSuccess(503));
}

void test_isHTTPSuccess_negative_fails(void) {
  TEST_ASSERT_FALSE(isHTTPSuccess(-1));
  TEST_ASSERT_FALSE(isHTTPSuccess(-11));
  TEST_ASSERT_FALSE(isHTTPSuccess(0));
}

void test_isHTTPSuccess_below_200_fails(void) {
  TEST_ASSERT_FALSE(isHTTPSuccess(100));
  TEST_ASSERT_FALSE(isHTTPSuccess(199));
}

// ─── preparePayload ─────────────────────────────────────────────────────────

void test_preparePayload_format(void) {
  VehicleData v;
  vehicleDataInit(v);
  strcpy(v.vin, "LGXCE4CB1R0123456");
  v.soc = 65;
  v.odometer = 7703.3f;
  v.batteryV = 29.8f;
  v.currentA = 2.3f;

  char json[512];
  time_t t = 1767225600; // 2026-01-01T00:00:00Z
  preparePayload(json, sizeof(json), v, t);

  TEST_ASSERT_EQUAL_STRING(
    "{\"vin\":\"LGXCE4CB1R0123456\",\"timestamp\":\"2026-01-01T00:00:00Z\","
    "\"odometer\":7703.3,\"battery_soc\":65,\"battery_v\":29.8,\"current_a\":2.3}",
    json);
}

void test_preparePayload_uninit_fields(void) {
  VehicleData v;
  vehicleDataInit(v);
  strcpy(v.vin, "VIN123");
  v.soc = 48;
  v.odometer = 100.0f;

  char json[512];
  time_t t = 1767225600;
  preparePayload(json, sizeof(json), v, t);

  // batteryV and currentA should be -1.0 (init sentinel)
  TEST_ASSERT_NOT_NULL(strstr(json, "\"battery_v\":-1.0"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"current_a\":-1.0"));
}

void test_preparePayload_buffer_too_small(void) {
  VehicleData v;
  vehicleDataInit(v);
  strcpy(v.vin, "LGXCE4CB1R0123456");
  v.soc = 65;
  v.odometer = 7703.3f;
  v.batteryV = 29.8f;
  v.currentA = 2.3f;

  char json[10]; // way too small
  time_t t = 1767225600;
  int written = preparePayload(json, sizeof(json), v, t);

  // snprintf returns what WOULD have been written (> bufLen)
  TEST_ASSERT_GREATER_THAN(10, written);
}

// ─── Runner ─────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  UNITY_BEGIN();

  RUN_TEST(test_isHTTPSuccess_2xx);
  RUN_TEST(test_isHTTPSuccess_3xx);
  RUN_TEST(test_isHTTPSuccess_4xx);
  RUN_TEST(test_isHTTPSuccess_5xx_fails);
  RUN_TEST(test_isHTTPSuccess_negative_fails);
  RUN_TEST(test_isHTTPSuccess_below_200_fails);
  RUN_TEST(test_preparePayload_format);
  RUN_TEST(test_preparePayload_uninit_fields);
  RUN_TEST(test_preparePayload_buffer_too_small);

  return UNITY_END();
}
