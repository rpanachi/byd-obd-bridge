#include <unity.h>
#include "obd_parser.h"

void setUp(void) {}
void tearDown(void) {}

// ─── parseHexByte ───────────────────────────────────────────────────────────

void test_parseHexByte_valid(void) {
  TEST_ASSERT_EQUAL(0x00, parseHexByte("00"));
  TEST_ASSERT_EQUAL(0x4E, parseHexByte("4E"));
  TEST_ASSERT_EQUAL(0xFF, parseHexByte("FF"));
  TEST_ASSERT_EQUAL(0xAB, parseHexByte("AB"));
  TEST_ASSERT_EQUAL(0x0A, parseHexByte("0A"));
}

void test_parseHexByte_lowercase(void) {
  TEST_ASSERT_EQUAL(0xAB, parseHexByte("ab"));
  TEST_ASSERT_EQUAL(0xFF, parseHexByte("ff"));
}

void test_parseHexByte_invalid(void) {
  TEST_ASSERT_EQUAL(-1, parseHexByte(NULL));
  TEST_ASSERT_EQUAL(-1, parseHexByte(""));
  TEST_ASSERT_EQUAL(-1, parseHexByte("G0"));
  TEST_ASSERT_EQUAL(-1, parseHexByte("0"));
  TEST_ASSERT_EQUAL(-1, parseHexByte("ZZ"));
}

// ─── parseLittleEndian ──────────────────────────────────────────────────────

void test_parseLittleEndian_1byte(void) {
  TEST_ASSERT_EQUAL(0x4E, parseLittleEndian("4E", 1));
  TEST_ASSERT_EQUAL(0x00, parseLittleEndian("00", 1));
  TEST_ASSERT_EQUAL(0xFF, parseLittleEndian("FF", 1));
}

void test_parseLittleEndian_2bytes(void) {
  TEST_ASSERT_EQUAL(298, parseLittleEndian("2A01", 2));     // 0x012A
  TEST_ASSERT_EQUAL(5023, parseLittleEndian("9F13", 2));    // 0x139F
  TEST_ASSERT_EQUAL(0, parseLittleEndian("0000", 2));
  TEST_ASSERT_EQUAL(65535, parseLittleEndian("FFFF", 2));
}

void test_parseLittleEndian_3bytes(void) {
  TEST_ASSERT_EQUAL(76900, parseLittleEndian("642C01", 3)); // 0x012C64
  TEST_ASSERT_EQUAL(0, parseLittleEndian("000000", 3));
}

void test_parseLittleEndian_invalid(void) {
  TEST_ASSERT_EQUAL(-1, parseLittleEndian("ZZ", 1));
  TEST_ASSERT_EQUAL(-1, parseLittleEndian("FFZZ", 2));
}

// ─── findUDSData ────────────────────────────────────────────────────────────

void test_findUDSData_valid(void) {
  const char* resp = "620005" "4E";
  const char* d = findUDSData(resp);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL('4', d[0]);
  TEST_ASSERT_EQUAL('E', d[1]);
}

void test_findUDSData_with_header_prefix(void) {
  const char* resp = "7891620008" "2A01";
  const char* d = findUDSData(resp);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL('2', d[0]);
}

void test_findUDSData_no_match(void) {
  TEST_ASSERT_NULL(findUDSData("NO DATA"));
  TEST_ASSERT_NULL(findUDSData("ERROR"));
  TEST_ASSERT_NULL(findUDSData(""));
}

// ─── VehicleData init ───────────────────────────────────────────────────────

void test_vehicleDataInit(void) {
  VehicleData v;
  v.soc = 50;
  v.odometer = 1000;
  vehicleDataInit(v);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.soc);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.odometer);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.batteryV);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.currentA);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.auxBattV);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.capacity);
  TEST_ASSERT_EQUAL('\0', v.vin[0]);
}

// ─── PID Table ──────────────────────────────────────────────────────────────

void test_pid_table_count(void) {
  TEST_ASSERT_EQUAL(5, PID_COUNT);
}

void test_pid_table_headers(void) {
  TEST_ASSERT_EQUAL_STRING("781", PID_TABLE[0].header); // SOC
  TEST_ASSERT_EQUAL_STRING("781", PID_TABLE[1].header); // Voltage
  TEST_ASSERT_EQUAL_STRING("781", PID_TABLE[2].header); // Current
  TEST_ASSERT_EQUAL_STRING("743", PID_TABLE[3].header); // Odometer
  TEST_ASSERT_EQUAL_STRING("743", PID_TABLE[4].header); // Capacity
}

void test_pid_table_commands(void) {
  TEST_ASSERT_EQUAL_STRING("220005", PID_TABLE[0].cmd);
  TEST_ASSERT_EQUAL_STRING("220008", PID_TABLE[1].cmd);
  TEST_ASSERT_EQUAL_STRING("220009", PID_TABLE[2].cmd);
  TEST_ASSERT_EQUAL_STRING("220026", PID_TABLE[3].cmd);
  TEST_ASSERT_EQUAL_STRING("220104", PID_TABLE[4].cmd);
}

// ─── applyPIDResponse ───────────────────────────────────────────────────────

void test_applyPID_soc(void) {
  VehicleData v;
  vehicleDataInit(v);
  // SOC: 1 byte, divisor 1, offset 0 → 0x4E = 78%
  TEST_ASSERT_TRUE(applyPIDResponse(v, PID_TABLE[0], "620005" "4E"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 78.0f, v.soc);
}

void test_applyPID_voltage(void) {
  VehicleData v;
  vehicleDataInit(v);
  // Voltage: 2 bytes LE, divisor 10 → 298 → 29.8V
  TEST_ASSERT_TRUE(applyPIDResponse(v, PID_TABLE[1], "620008" "2A01"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 29.8f, v.batteryV);
}

void test_applyPID_current(void) {
  VehicleData v;
  vehicleDataInit(v);
  // Current: 2 bytes LE, divisor 10, offset -5000 → 5023 → 2.3A
  TEST_ASSERT_TRUE(applyPIDResponse(v, PID_TABLE[2], "620009" "9F13"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.3f, v.currentA);
}

void test_applyPID_current_negative(void) {
  VehicleData v;
  vehicleDataInit(v);
  // Current when charging: raw < 5000 → negative current
  // raw = 4900 (0x1324 LE = "2413") → (4900 - 5000) / 10 = -10.0A
  TEST_ASSERT_TRUE(applyPIDResponse(v, PID_TABLE[2], "620009" "2413"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -10.0f, v.currentA);
}

void test_applyPID_odometer(void) {
  VehicleData v;
  vehicleDataInit(v);
  // Odometer: 3 bytes LE, divisor 10 → 76900 → 7690.0 km
  TEST_ASSERT_TRUE(applyPIDResponse(v, PID_TABLE[3], "620026" "642C01"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7690.0f, v.odometer);
}

void test_applyPID_capacity(void) {
  VehicleData v;
  vehicleDataInit(v);
  // Capacity: 2 bytes LE, divisor 100 → 5000 → 50.00 Ah
  TEST_ASSERT_TRUE(applyPIDResponse(v, PID_TABLE[4], "620104" "8813"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, v.capacity);
}

void test_applyPID_no_data(void) {
  VehicleData v;
  vehicleDataInit(v);
  TEST_ASSERT_FALSE(applyPIDResponse(v, PID_TABLE[0], "NO DATA"));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.soc);
}

void test_applyPID_isolates_fields(void) {
  VehicleData v;
  vehicleDataInit(v);
  // Writing SOC should not affect other fields
  applyPIDResponse(v, PID_TABLE[0], "620005" "4E");
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 78.0f, v.soc);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.odometer);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, v.batteryV);
}

// ─── parseVIN ───────────────────────────────────────────────────────────────

void test_parseVIN_multiframe(void) {
  // L=4C G=47 X=58 C=43 E=45 4=34 C=43 B=42 1=31 R=52 0=30 1=31 2=32 3=33 4=34 5=35 6=36
  const char* resp =
    "7E8" "1014490201" "4C47584345"
    "7E8" "21" "3443423152303132"
    "7E8" "22" "3334353600000000";

  char vin[18] = {0};
  int len = parseVIN(resp, vin, sizeof(vin));
  TEST_ASSERT_EQUAL(17, len);
  TEST_ASSERT_EQUAL_STRING("LGXCE4CB1R0123456", vin);
}

void test_parseVIN_empty_response(void) {
  char vin[18] = {0};
  int len = parseVIN("NO DATA", vin, sizeof(vin));
  TEST_ASSERT_EQUAL(0, len);
  TEST_ASSERT_EQUAL_STRING("", vin);
}

void test_parseVIN_partial_response(void) {
  const char* resp = "7E8" "1014490201" "4C47584345";
  char vin[18] = {0};
  int len = parseVIN(resp, vin, sizeof(vin));
  TEST_ASSERT_GREATER_THAN(0, len);
  TEST_ASSERT_EQUAL('L', vin[0]);
}

// ─── Runner ─────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  UNITY_BEGIN();

  // Hex parsing
  RUN_TEST(test_parseHexByte_valid);
  RUN_TEST(test_parseHexByte_lowercase);
  RUN_TEST(test_parseHexByte_invalid);
  RUN_TEST(test_parseLittleEndian_1byte);
  RUN_TEST(test_parseLittleEndian_2bytes);
  RUN_TEST(test_parseLittleEndian_3bytes);
  RUN_TEST(test_parseLittleEndian_invalid);

  // UDS response parsing
  RUN_TEST(test_findUDSData_valid);
  RUN_TEST(test_findUDSData_with_header_prefix);
  RUN_TEST(test_findUDSData_no_match);

  // Vehicle data
  RUN_TEST(test_vehicleDataInit);

  // PID table
  RUN_TEST(test_pid_table_count);
  RUN_TEST(test_pid_table_headers);
  RUN_TEST(test_pid_table_commands);

  // PID response processing (end-to-end: raw response → parsed value)
  RUN_TEST(test_applyPID_soc);
  RUN_TEST(test_applyPID_voltage);
  RUN_TEST(test_applyPID_current);
  RUN_TEST(test_applyPID_current_negative);
  RUN_TEST(test_applyPID_odometer);
  RUN_TEST(test_applyPID_capacity);
  RUN_TEST(test_applyPID_no_data);
  RUN_TEST(test_applyPID_isolates_fields);

  // VIN parsing
  RUN_TEST(test_parseVIN_multiframe);
  RUN_TEST(test_parseVIN_empty_response);
  RUN_TEST(test_parseVIN_partial_response);

  return UNITY_END();
}
