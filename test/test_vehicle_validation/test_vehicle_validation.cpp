#include <unity.h>
#include "vehicle_validation.h"

void setUp(void) {}
void tearDown(void) {}

static VehicleData reading(float soc, float odometer, float batteryV, float currentA) {
  VehicleData v;
  vehicleDataInit(v);
  v.soc = soc;
  v.odometer = odometer;
  v.batteryV = batteryV;
  v.currentA = currentA;
  return v;
}

// ─── Rows seen in production ────────────────────────────────────────────────

void test_row_read_before_ready_is_rejected(void) {
  // 2026-08-11T15:32:23: BMS answered SOC/voltage, VCU had not synced the
  // odometer yet, contactors still open (current exactly 0)
  TEST_ASSERT_FALSE(isVehicleDataValid(reading(80, 0.0f, 30.0f, 0.0f)));
}

void test_row_long_trip_is_accepted(void) {
  // 2026-08-30T21:30:16: 254 km trip, arrived at 9%, lowest pack voltage seen
  TEST_ASSERT_TRUE(isVehicleDataValid(reading(9, 11354.4f, 28.7f, 5.7f)));
}

// ─── isVehicleDataValid ─────────────────────────────────────────────────────

void test_valid_typical_reading(void) {
  TEST_ASSERT_TRUE(isVehicleDataValid(reading(65, 7703.3f, 29.9f, 1.2f)));
}

void test_unread_fields_are_rejected(void) {
  VehicleData v;
  vehicleDataInit(v);   // every field is the -1 sentinel
  TEST_ASSERT_FALSE(isVehicleDataValid(v));
  TEST_ASSERT_FALSE(isVehicleDataValid(reading(-1, 7703.3f, 29.9f, 1.2f)));
  TEST_ASSERT_FALSE(isVehicleDataValid(reading(65, -1, 29.9f, 1.2f)));
  TEST_ASSERT_FALSE(isVehicleDataValid(reading(65, 7703.3f, -1, 1.2f)));
}

void test_soc_range(void) {
  TEST_ASSERT_FALSE(isSOCValid(0));     // BMS not initialised
  TEST_ASSERT_TRUE(isSOCValid(1));
  TEST_ASSERT_TRUE(isSOCValid(100));
  TEST_ASSERT_FALSE(isSOCValid(101));
  TEST_ASSERT_FALSE(isSOCValid(255));   // 0xFF, a typical "no value" byte
}

void test_odometer_range(void) {
  TEST_ASSERT_FALSE(isOdometerValid(0.0f));        // VCU not synced
  TEST_ASSERT_TRUE(isOdometerValid(0.1f));
  TEST_ASSERT_TRUE(isOdometerValid(11440.1f));
  TEST_ASSERT_TRUE(isOdometerValid(999999.9f));
  TEST_ASSERT_FALSE(isOdometerValid(1000000.0f));
  TEST_ASSERT_FALSE(isOdometerValid(1118481.0f));  // "AAAAAA" padding parsed as data
  TEST_ASSERT_FALSE(isOdometerValid(1677721.5f));  // "FFFFFF"
}

void test_battery_voltage_range(void) {
  TEST_ASSERT_FALSE(isBatteryVValid(0.0f));
  TEST_ASSERT_FALSE(isBatteryVValid(19.9f));
  TEST_ASSERT_TRUE(isBatteryVValid(20.0f));
  TEST_ASSERT_TRUE(isBatteryVValid(28.7f));
  TEST_ASSERT_TRUE(isBatteryVValid(30.3f));
  TEST_ASSERT_TRUE(isBatteryVValid(40.0f));
  TEST_ASSERT_FALSE(isBatteryVValid(40.1f));
  TEST_ASSERT_FALSE(isBatteryVValid(4360.0f));     // SOC reply parsed as voltage
}

void test_current_does_not_affect_validity(void) {
  // Current is 0 with the contactors open and negative while charging; both
  // are real states, so the current alone never invalidates a reading
  TEST_ASSERT_TRUE(isVehicleDataValid(reading(80, 10815.8f, 30.0f, 0.0f)));
  TEST_ASSERT_TRUE(isVehicleDataValid(reading(80, 10815.8f, 30.0f, -16.0f)));
  TEST_ASSERT_TRUE(isVehicleDataValid(reading(80, 10815.8f, 30.0f, -1.0f)));
}

// ─── vehicleDataAgrees ──────────────────────────────────────────────────────

void test_agrees_identical(void) {
  VehicleData a = reading(46, 11435.3f, 29.5f, 4.9f);
  VehicleData b = reading(46, 11435.3f, 29.4f, 6.8f);   // voltage/current may differ
  TEST_ASSERT_TRUE(vehicleDataAgrees(a, b));
}

void test_agrees_within_tolerance(void) {
  VehicleData a = reading(46, 11435.3f, 29.5f, 4.9f);
  TEST_ASSERT_TRUE(vehicleDataAgrees(a, reading(46, 11435.4f, 29.5f, 4.9f)));  // car creeping
  TEST_ASSERT_TRUE(vehicleDataAgrees(a, reading(45, 11435.3f, 29.5f, 4.9f)));  // SOC ticked down
  TEST_ASSERT_TRUE(vehicleDataAgrees(a, reading(47, 11435.3f, 29.5f, 4.9f)));
}

void test_disagrees_on_odometer(void) {
  VehicleData a = reading(46, 11435.3f, 29.5f, 4.9f);
  TEST_ASSERT_FALSE(vehicleDataAgrees(a, reading(46, 0.0f, 29.5f, 4.9f)));     // second read hit an unsynced VCU
  TEST_ASSERT_FALSE(vehicleDataAgrees(reading(46, 0.0f, 29.5f, 4.9f), a));     // first read did
  TEST_ASSERT_FALSE(vehicleDataAgrees(a, reading(46, 11435.8f, 29.5f, 4.9f)));
}

void test_disagrees_on_soc(void) {
  VehicleData a = reading(46, 11435.3f, 29.5f, 4.9f);
  TEST_ASSERT_FALSE(vehicleDataAgrees(a, reading(44, 11435.3f, 29.5f, 4.9f)));
  TEST_ASSERT_FALSE(vehicleDataAgrees(a, reading(100, 11435.3f, 29.5f, 4.9f)));
}

// ─── Runner ─────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  UNITY_BEGIN();

  // Rows seen in production
  RUN_TEST(test_row_read_before_ready_is_rejected);
  RUN_TEST(test_row_long_trip_is_accepted);

  // Plausibility
  RUN_TEST(test_valid_typical_reading);
  RUN_TEST(test_unread_fields_are_rejected);
  RUN_TEST(test_soc_range);
  RUN_TEST(test_odometer_range);
  RUN_TEST(test_battery_voltage_range);
  RUN_TEST(test_current_does_not_affect_validity);

  // Agreement between consecutive reads
  RUN_TEST(test_agrees_identical);
  RUN_TEST(test_agrees_within_tolerance);
  RUN_TEST(test_disagrees_on_odometer);
  RUN_TEST(test_disagrees_on_soc);

  return UNITY_END();
}
