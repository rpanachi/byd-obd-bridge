# iCar BLE Bridge for BYD Dolphin Mini

ESP32 project that reads vehicle data from a BYD EV via an iCar (ELM327-compatible) BLE OBD2 adapter and POSTs it as JSON to a configurable URL. Could work on other BYD EV models, but has only been tested on a **BYD Dolphin Mini**.

## How It Works

The ESP32 runs a one-shot operation in two phases (BLE and WiFi can't share the radio reliably on ESP32):

1. **Phase 1 — BLE Read**: Connect to iCar adapter, read OBD2 data, save to RTC memory, restart
2. **Phase 2 — WiFi POST**: Connect to WiFi, POST JSON payload, enter deep sleep

The car must be in **Ready mode** (ignition on, HV system active) for valid readings. If the odometer can't be read (car is off), the ESP32 skips the POST and goes directly to deep sleep.

### JSON Payload

```json
{"vin": "LXXXXXXXXXXXXXXXXX", "timestamp": "2026-03-25T12:30:00Z", "odometer": 7703.3, "battery": 65}
```

| Field | Type | Description |
|-------|------|-------------|
| `vin` | string | 17-character Vehicle Identification Number |
| `timestamp` | string | ISO 8601 UTC datetime, obtained via NTP after WiFi connects |
| `odometer` | decimal | Odometer reading in km |
| `battery` | integer | Battery state of charge (%) |

## Readings

| Parameter | ECU | PID | Format | Status |
|-----------|-----|-----|--------|--------|
| Battery SOC (%) | `781` | `0005` | 1 byte, direct % | **Confirmed** |
| Odometer (km) | `743` | `0026` | 3 bytes LE, /10 | **Confirmed** |
| VIN | `7DF` | `09 02` | Multi-frame, 17 ASCII chars | **Confirmed** |
| Battery Voltage | `781` | `0008` | 2 bytes LE, /10 | **Confirmed** |
| Battery Current (A) | `781` | `0009` | 2 bytes LE, (val-5000)/10 | **Confirmed** |
| Battery Capacity (Ah) | `743` | `0104` | 2 bytes LE, /100 | **Confirmed** (50.00 Ah) |
| 12V Battery (V) | — | `AT RV` | ELM327 internal | **Confirmed** |

## Hardware

- **ESP32** dev board (any variant with BLE support)
- **iCar Pro** (Vgate) or compatible ELM327 BLE 4.0+ adapter
- **BYD Dolphin Mini** (e-Platform 3.0, LFP Blade Battery, 38 kWh)

## Project Structure

```
src/main.cpp              # Application logic (BLE, WiFi, phases)
include/obd_parser.h      # OBD parsing, PID table, VIN decoder
include/json_builder.h    # JSON payload and ISO 8601 formatting
include/secrets.h         # WiFi/URL credentials (not committed)
test/test_obd_parser/     # Tests for OBD parsing logic
test/test_json_builder/   # Tests for JSON payload building
```

## Setup

1. Install [PlatformIO](https://platformio.org/)
2. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your values:
   ```c
   #define WIFI_SSID "your-wifi-ssid"
   #define WIFI_PASS "your-wifi-password"
   #define POST_URL  "https://your-server.com/api/vehicle"
   ```
3. Build and upload:
   ```bash
   pio run -t upload
   ```

## Tests

Tests run natively on your machine (no ESP32 required):

```bash
pio test -e native
```

## Serial Output

```
=== iCar BLE Bridge for BYD Dolphin Mini ===

[BOOT] Phase 1 — BLE Read
[1/5] Connecting to iCar adapter...
[1/5] Connected to iCar Pro
[2/5] Reading vehicle data...
[2/5] Read attempt 1/5...
[2/5] Read successful — SOC=65% Odometer=7703.3 km

========================================
   BYD Dolphin Mini - Vehicle Data
========================================
  VIN:              LXXXXXXXXXXXXXXXXX
  Battery SOC:      65%
  Odometer:         7703.3 km
  Battery Voltage:  29.9 V
  Battery Current:  0.8 A
  Battery Capacity: 50.00 Ah
  12V Battery:      13.7 V
========================================

[3/5] Disconnecting from iCar...
[3/5] Disconnected from iCar
[3/5] Restarting for WiFi phase...

=== iCar BLE Bridge for BYD Dolphin Mini ===

[BOOT] Phase 2 — WiFi POST
[4/5] Data from OBD: SOC=65% Odometer=7703.3 km VIN=LXXXXXXXXXXXXXXXXX
[4/5] Connecting to WiFi...
[WiFi] IP: 192.168.1.234
[4/5] Connected to WiFi
[4/5] POST attempt 1/5...
[HTTP] POST https://your-server.com/api/vehicle
[HTTP] Payload: {"vin":"LXXXXXXXXXXXXXXXXX","timestamp":"2026-03-25T12:30:00Z","odometer":7703.3,"battery":65}
[HTTP] Response: 200
[4/5] Data sent successfully
[5/5] Disconnecting from WiFi...
[5/5] Disconnected from WiFi
[SLEEP] Entering deep sleep...
```

## Retry & Error Handling

| Step | Retries | Timeout | On failure |
|------|---------|---------|------------|
| BLE connection | 3 | 15s scan | Deep sleep |
| OBD read | 5 | 10s between retries | Deep sleep |
| WiFi connection | 3 | 30s each | Deep sleep |
| HTTP POST | 5 | 3s between retries | Deep sleep |

If the car is off (odometer not readable), the ESP32 skips WiFi/POST and goes directly to deep sleep.

## BLE Compatibility

| Profile | Service UUID | TX (write) | RX (notify) |
|---------|-------------|------------|-------------|
| iCar Pro (Vgate) | `18F0` | `2AF1` | `2AF0` |
| ELM327 Clone (HM-10) | `FFE0` | `FFE1` | `FFE1` |

## Protocol Details

- **CAN bus:** 500 kbps, 11-bit identifiers (ELM327 protocol 6)
- **Diagnostic protocol:** UDS (ISO 14229) service 0x22 (ReadDataByIdentifier)

### ECU Map

| Header | Response | Role | Key PIDs |
|--------|----------|------|----------|
| `781` | `789` | **BMS** — Battery Management System | SOC, voltage, current |
| `743` | `74B` | **VCU** — Vehicle Control Unit | Odometer, capacity |
| `7E0` | `7E8` | Gateway (mirror of 743) | Same as 743 |
| `766` | `76E` | Secondary ECU | 9 PIDs, live counters |
| `645` | `644` | BMS broadcast | Cell voltages (3.3V LFP) |
| `7DF` | varies | OBD2 broadcast | VIN |

### Data Encoding

All multi-byte values use **little-endian** byte order (no spaces with `ATS0`):

| PID | Bytes | Formula | Example |
|-----|-------|---------|---------|
| `0005` (SOC) | 1 | direct % | `4E` → 78% |
| `0026` (Odo) | 3 | LE / 10 → km | `642C01` → 76900 → 7690.0 km |
| `0008` (Volt) | 2 | LE / 10 → V | `2A01` → 298 → 29.8 V |
| `0009` (Curr) | 2 | (LE - 5000) / 10 → A | `9F13` → 5023 → 2.3 A |
| `0104` (Cap) | 2 | LE / 100 → Ah | `8813` → 5000 → 50.00 Ah |

## Notes

- The iCar adapter goes to sleep when there's no CAN bus activity. Turn the car ignition on to wake it.
- The iCar adapter must **not** be paired via OS Bluetooth settings — BLE connection is handled by the ESP32.
- The BYD Dolphin Mini does **not** use header `7E7` for BMS (unlike the standard Dolphin/Atto 3). The BMS is at header `781`.
- Standard OBD2 service 0x01 PIDs are not supported by this vehicle (returns `7F 01 22`).
- PID `001F` on ECU 743/7E0 is a running counter, **not** SOC.
- ESP32 BLE and WiFi can't reliably share the radio in the same boot cycle. The two-phase restart approach solves this.
