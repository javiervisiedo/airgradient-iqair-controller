---
name: matter-smart-home
description: >-
  Use this skill when working on the Matter over Wi-Fi smart-home bridge
  (Apple Home, Google Home, and Home Assistant via Multi-Admin): composite
  Matter endpoints (Air Purifier/Fan, Air Quality + Temp/RH + BooleanState
  fault, Night Quiet Lock switch), FanControl Auto vs Manual percent handling,
  CHIP task threading (ScheduleWork / g_publishing), BLE commissioning and RAM
  release, and verifying Home Assistant entities via REST.
---

# Matter Smart Home Integration (Multi-Admin)

**Architecture decision (2026-11-09, SRS v2.2 §2.1 & §5.1):** The device exposes a
native **Matter over Wi-Fi** node (Arduino-ESP32 Matter / ESP-Matter SDK on
`pioarduino 55.03.312-1`) for simultaneous **Apple Home**, **Google Home**, and
future **Home Assistant** via Multi-Admin. MQTT discovery was superseded and is
not used.

Implementation: `include/matter_ha_bridge.h`, `src/matter_ha_bridge.cpp`.

## Endpoints & Data Model

Created in order in `MatterHaBridge::begin()`:

| Endpoint | Type / IDs | Clusters / Features | Notes |
| :--- | :--- | :--- | :--- |
| **EP1** | `MatterFan` (`0x002B`) + `air_purifier` (`0x002D` via `-DIAQ_MATTER_AIR_PURIFIER_TYPE=1`) | `OnOff`, `FanControl` (`FAN_MODE_SEQ_OFF_LOW_MED_HIGH_AUTO`, 6-step percent slider) | Slider / mode callbacks post `SET_AUTO` or `SET_MANUAL` to `g_cmd_q`. Percent ↔ speed: 0→0%, 1→17%, 2→33%, 3→50%, 4→67%, 5→83%, 6→100% (`speed_to_percent` / `percent_to_speed` in `iaq_algorithm.cpp`). |
| **EP2** | `AirQualityEndpoint` (`0x002C`) + `temperature_sensor` (`0x0302`) + `humidity_sensor` (`0x0307`) | `AirQuality` (Fair/Moderate/VeryPoor/ExtremelyPoor), numeric `PM2.5` (µg/m³) & `CO2` (ppm), `LevelIndication` for `TVOC` & `NO2`, `TemperatureMeasurement`, `RelativeHumidityMeasurement`, `BooleanState` (fault) | TVOC and NOx from AirGradient are Sensirion *indices* (1–500), so they use `LevelIndication` (Low/Medium/High/Critical) rather than fake ppb/ppm concentrations. All sensor values are set `null` / `Unknown` (`0`) when `!have_reading || alarm_sensor_unreachable`; `BooleanState` mirrors `alarm_sensor_unreachable`. |
| **EP3** | `MatterOnOffPlugin` (`0x010A`) `"Purifier Night Quiet Lock"` | `OnOff` | Mirrors `NIGHT_FORCE`. Toggling posts `SET_NIGHT_FORCE` to `g_cmd_q`. |

### Critical `FanControl` Invariant in Auto Mode

When `operating_mode != MANUAL` (`FAN_MODE_AUTO`), the Matter `FanControl` server
keeps `PercentSetting` null. **Never call `g_fan.setSpeedPercent()` in Auto mode** —
writing `PercentSetting` flips `FanMode` out of Auto (`0` → Off). Instead, update
`FanControl::Attributes::PercentCurrent::Id` directly via `attribute::update(...)`
(see `MatterHaBridge::publish_matter()`).

In `MANUAL` mode, call `g_fan.setMode(fm)` *before* `g_fan.setSpeedPercent(...)` so
the exact 6-step percent overrides the coarse Low/Med/High preset percent.

## Threading & Locking Rules

- **CHIP task → `loop()`:** Matter callbacks (`onChangeMode`, `onChangeSpeedPercent`,
  `onChangeOnOff`) run in the CHIP task. They MUST NEVER touch IR, LED, NVS, or
  control state directly — only check `if (g_publishing) return true;` and post a
  `BridgeCmd` to `g_cmd_q`.
- **`loop()` → CHIP task:** `MatterHaBridge::publish()` copies the `StatusSnapshot`
  under `status_mutex_` and schedules `publish_work` on the CHIP task via
  `chip::DeviceLayer::PlatformMgr().ScheduleWork(...)` (coalesced by `g_work_scheduled`).
  Never block `loop()` waiting on the CHIP stack lock.
- **`net_task` → CHIP stack:** `matter_utc()` uses `TryLockChipStack()` so it never
  blocks `net_task` or triggers the 15 s TWDT.

## Commissioning, BLE Memory, and Partitions

- **Partition table:** `partitions_8MB_matter.csv` (dual 3.5 MB OTA slots + 64 KB
  `nvs` + 24 KB `fctry` + 872 KB `littlefs`).
- **BLE SRAM reclamation:** `Matter.setBLEMemoryReleaseEnabled(true)` releases NimBLE
  RAM (~65 KB) automatically once commissioned.
- **Test attestation:** Uses Arduino-ESP32 Matter's default test DAC/credentials.
  Apple Home and Google Home will warn about an *uncertified accessory* — accept
  during pairing.
- **Multi-Admin flow:** Commission in Apple Home via the QR URL / 11-digit manual
  pairing code printed over USB-CDC (`115200 baud`), then open *Turn On Pairing
  Mode* in Apple Home to share with Google Home or Home Assistant.
- **Factory reset:** Button hold ≥ 10 s (`main.cpp`) clears the REST API token in
  NVS (`iaqauth`) and calls `Matter.decommission()` (erases Matter fabrics and
  reboots into BLE commissioning).
- **Wokwi limitation:** Wokwi does not simulate Bluetooth, so Matter BLE
  commissioning cannot be tested in Wokwi. Use `include/secrets.h` for Wi-Fi in
  simulation.

## Known Open Items

1. **Time Synchronization cluster (`0x0038`):** `matter_utc()` queries cluster
   `0x0038` on endpoint 0, which is not instantiated by the current node; Tier 3
   clock sync therefore falls through to HTTP `Date`. Adding the cluster server
   on EP0 is needed if Matter hub time is desired.
2. **Hardware commissioning & Multi-Admin bring-up (`TC-01`):** Verify Apple Home,
   Google Home, and Home Assistant UI rendering for composite EP2 (`AirQuality`,
   `LevelIndication`, `BooleanState`) and EP1 (`air_purifier` `0x002D` device type).
3. **Host test coverage:** Add unit tests for `matter_air_quality_from_pm25`,
   `matter_level_from_tvoc_index`, and `matter_level_from_nox_index` (currently
   only `speed_to_percent` / `percent_to_speed` are covered in `test_main.cpp`).

## Verification with Home Assistant (once paired via Multi-Admin)

If Home Assistant is added to the Matter fabric, you can inspect its entities from
the host using `scripts/ha_query.py` (`HA_URL` and `HA_TOKEN` in env or `.env`):

```bash
python3 .agents/skills/matter-smart-home/scripts/ha_query.py ping
python3 .agents/skills/matter-smart-home/scripts/ha_query.py states fan.
python3 .agents/skills/matter-smart-home/scripts/ha_query.py state fan.atom_s3_iaq_purifier_controller
```
