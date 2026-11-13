---
name: air-quality-control-logic
description: >-
  Use this skill when implementing, changing, or testing the decision logic that
  turns AirGradient readings into IQAir fan speeds: how to work from the
  requirements specs, keeping the policy host-testable, deriving tests from the
  specs, handling spec changes, AQI for display, and override handling.
---

# Air-Quality Control Logic

## Requirements live in the specs, not here

The control policy (inputs, thresholds, modes, smoothing, timing, watchdog,
volume scaling, overrides) is defined across two authoritative specs:
- **`docs/automated_iaq_multigas_fan_controller_spec_v3.md`** (v3.0.1: SMA-5,
  PM2.5 hysteresis, 2-tier multi-gas floor, asymmetric attack/decay, §3.1 watchdog caps)
- **`docs/main_requirement_spec.md`** (SRS v2.2: §2.4 `room_volume_m3` scaling `ρ(V)` / `T_hold(V)`,
  §3.1 fault/frozen-reading rules, §3.3 `AUTO`/`MANUAL`/`NIGHT_FORCE` and button cycling)

This skill only says *how* to work with them. Don't copy requirement values into
skills, rules, or `GEMINI.md`, because the specs get revised independently.

Implementation: `include/iaq_algorithm.h`, `src/iaq_algorithm.cpp`, configured
via `AppConfig` (`include/sys_config.h`). Tests: `test/test_algorithm/test_main.cpp`.

## Workflow

1. **Read both current specs first, every time.** Never work from memory or from an
   older conversation. If a file is missing or empty, stop and tell the user.
2. **Implement exactly what the specs say.** Don't tune thresholds, smoothing, or
   timings on your own. If something looks wrong, propose a spec change to the user.
3. **Find the gaps before coding.** List anything the specs leave undefined (equality
   edges, boundary times, behaviour without a valid clock, interaction with overrides)
   and ask the user. The answers belong in the specs, not in a skill.
4. **Derive tests from the specs.** Every threshold, mode boundary, timer, and
   fail-safe rule gets a unit test (`TC-01..TC-09` + algorithm cases), including
   equality edges and transitions. Name each test after the spec case or section it
   covers.
5. **When a spec changes:** diff the old and new spec, update the affected tests
   first, then the code.

## Engineering constraints (independent of the spec's values)

- **Pure C++17 (`UNIT_TEST` host-testable):** `iaq_algorithm.h/.cpp` must have zero
  Arduino or ESP-IDF headers. Readings, validity flags, resolved `is_night`, and
  `now_ms` are passed in as arguments so `pio test -e native` tests `ControlCore`,
  `DualPathController`, `SensorFaultMonitor`, and `ModeController` directly.
- **Tick-aged window (`TickWindow`):** missed or failed polls must call
  `missed_tick()` so pre-outage SMA-5 samples age out over 5 minutes instead of
  sticking indefinitely.
- **Timing must survive `millis()` wraparound** (`now_ms - last_ms >= interval`).
- **Missing, invalid, or frozen data** never raises speed and never sends IR
  commands except for the explicit watchdog caps and recovery re-transmit in SRS §3.1.
- **Nothing commanded yet:** after reboot before the first valid reading or manual
  command, `ControlCore` holds no decision (`have_commanded_ == false`) and sends
  no IR command even if polls fail.

## Overrides and external commands

`ModeController` enforces precedence between `AUTO`, `MANUAL` (Matter FanControl,
REST `PATCH`/`POST`, button cycle), and `NIGHT_FORCE` (Matter Night Quiet Lock,
REST). `MANUAL` is exempt from watchdog caps, and switching back to `AUTO`
re-anchors `DualPathController` to `commanded_speed_` (`sync_current_speed`).
All speed changes flow through `ControlCore` → `IrNecPurifier`.

## AQI / Air Quality Display

`matter_air_quality_from_pm25()` in `src/matter_ha_bridge.cpp` maps PM2.5 to
Matter `AirQualityEnum` using the US EPA 2024 breakpoints in
[aqi_and_thresholds.md](references/aqi_and_thresholds.md). (Note: those pure
Matter mappers are not yet compiled in `[env:native]` tests.)
