# iCar BLE Bridge for BYD Dolphin Mini

ESP32 project that connects via Bluetooth Low Energy to an iCar (ELM327-compatible) OBD2 adapter plugged into a BYD Dolphin Mini and reads vehicle data to Serial output.

## Readings

| Parameter | ECU | PID | Format | Status |
|-----------|-----|-----|--------|--------|
| Battery SOC (%) | `781` | `0005` | 1 byte, direct % | **Confirmed** (78% matched dashboard) |
| Odometer (km) | `743` | `0026` | 3 bytes LE, /10 | **Confirmed** (7690.0 km matched dashboard) |
| VIN | `7DF` | `09 02` | Multi-frame, 17 ASCII chars | **Confirmed** (LXXXXXXXXXXXXXXXXX) |
| Battery Voltage | `781` | `0008` | 2 bytes LE | From Car Scanner log |
| Battery Current (A) | `781` | `0009` | 2 bytes LE, (val-5000)/10 | From Car Scanner log |
| Battery Capacity (Ah) | `743` | `0104` | 2 bytes LE, /100 | **Confirmed** (50.00 Ah) |
| 12V Battery (V) | — | `AT RV` | ELM327 internal | From Car Scanner log (13.7V) |

## Hardware

- **ESP32** dev board (any variant with BLE support)
- **iCar Pro** (Vgate) or compatible ELM327 BLE 4.0+ adapter
- **BYD Dolphin Mini** (e-Platform 3.0, LFP Blade Battery, 38 kWh)

## Build & Upload

Requires [PlatformIO](https://platformio.org/).

```bash
pio run              # Build
pio run -t upload    # Upload to ESP32
pio device monitor   # Monitor serial output (115200 baud)
```

## Serial Output

```
========================================
   BYD Dolphin Mini - Vehicle Data
========================================
  VIN:              LXXXXXXXXXXXXXXXXX
  Battery SOC:      78%
  Odometer:         7690.0 km
  Battery Voltage:  29.8 V
  Battery Current:  2.3 A
  Battery Capacity: 50.00 Ah
  12V Battery:      13.7 V
  Car State:        READY
========================================
```

## BLE Compatibility

The firmware auto-detects two common BLE profiles:

| Profile | Service UUID | TX (write) | RX (notify) |
|---------|-------------|------------|-------------|
| iCar Pro (Vgate) | `18F0` | `2AF1` | `2AF0` |
| ELM327 Clone (HM-10) | `FFE0` | `FFE1` | `FFE1` |

## Protocol Details

- **CAN bus:** 500 kbps, 11-bit identifiers (ELM327 protocol 6)
- **Diagnostic protocol:** UDS (ISO 14229) service 0x22 (ReadDataByIdentifier)
- **Init sequence:** `ATZ ATE0 ATH1 ATSP0 ATS0 ATM0 ATAT1` (matches Car Scanner app)

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

All multi-byte values use **little-endian** byte order (same byte position as in CAN frame, no spaces when `ATS0`):

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

## SOC Investigation History

Finding the SOC PID required extensive reverse-engineering:
- 5 phases of ECU scanning across headers 600-7FF
- CAN broadcast monitoring (ID 644 — cell voltages)
- Final breakthrough via Car Scanner app log analysis revealing ECU `781` (BMS) in the 780-78F range which was initially missed during manual scanning
