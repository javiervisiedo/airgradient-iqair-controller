---
trigger: glob
globs: src/**/*.cpp,src/**/*.h,include/**/*.h,lib/**/*.cpp,lib/**/*.h,test/**/*.cpp
---

# Firmware C++ Conventions (ESP32-S3, pioarduino / Arduino-ESP32 + ESP-IDF)

## Concurrency, tasks, and timing
- `loop()` (Core 1) must stay non-blocking (~200 Hz, `delay(5)`). No `delay()` over
  10 ms in `loop()` (startup USB-CDC enumeration delay is `setup()`-only). Never
  busy-wait on Wi-Fi or block `loop()` on network I/O or the CHIP stack lock.
- Use unsigned subtraction (`now - last >= interval`) for periodic timers and
  signed difference (`static_cast<int32_t>(now - deadline) >= 0`) for deadlines
  so `millis()` wraparound (~49.7 days) is safe.
- Blocking local HTTP / mDNS calls (`AgClient::poll` via `esp_http_client`,
  `ClockSync::try_http_date`, `mdns_query_a` / `mdns_query_ptr`) run only in
  `net_task` (Core 0) with explicit bounded timeouts (`cfg.timeout_ms`,
  `kMdnsTimeoutMs`).
- **Task Watchdog (15 s TWDT):** Both `loop()` and `net_task` are registered on
  `esp_task_wdt` (`trigger_panic = true`). Reset the TWDT between blocking
  discovery/HTTP steps in `net_task`. Note that `esp_http_server` (`httpd`) is
  **not** TWDT-registered — never call `esp_task_wdt_reset()` inside HTTP
  handlers (`ESP_ERR_NOT_FOUND`).
- **Cross-task communication:** Tasks (`loop()`, `net_task`, CHIP task, `httpd`)
  communicate only through FreeRTOS queues (`g_cmd_q`, `g_poll_q`), mutexes
  (`g_cfg_mutex`, `status_mutex_`, `auth_mutex_`), or `std::atomic` flags.
- **Matter / CHIP task rules (`matter_ha_bridge.cpp`):**
  - Expose the 2 composite endpoints in order: EP1 `AirPurifierEndpoint`
    (`0x002D` `air_purifier`) and EP2 `AirQualityEndpoint` (Air Quality + Temp/RH
    + `BooleanState` fault, reporting `null`/`Unknown` when unreachable).
  - Matter attribute callbacks (`AirPurifierEndpoint::attributeChangeCB`) run in
    the CHIP task: check `if (g_publishing) return true;` and only post a
    `BridgeCmd` to `g_cmd_q`. Never touch IR, LED, NVS, or `ControlCore`
    directly from callbacks.
  - Publishing from `loop()` to Matter must use
    `chip::DeviceLayer::PlatformMgr().ScheduleWork(...)` (coalesced by
    `g_work_scheduled`) so `loop()` never blocks on the CHIP lock.
  - Reading Matter state from `net_task` (e.g., `matter_utc()`) must use
    `TryLockChipStack()`, never a blocking lock.
  - `FanControl` invariant: in Auto mode (`AirPurifierEndpoint::set_auto`), keep
    `PercentSetting` null and update `PercentCurrent` via `updateAttributeVal`.
    In `MANUAL` (`AirPurifierEndpoint::set_manual`), update `FanMode` before
    `PercentSetting`.
- **NVS / LittleFS writes:** `loop()` is the **only** writer of persistent config
  (`config_save`, debounced by `kConfigSaveDebounceMs`) and the REST API token
  (`state_save_api_token`, retried until NVS succeeds). Only write NVS/`/config.json`
  when the serialized content actually changed (flash wear).

## Memory (512 KB internal SRAM, no PSRAM)
- Enable `Matter.setBLEMemoryReleaseEnabled(true)` so BLE RAM (~65 KB) is freed
  once commissioned.
- Never use Arduino `String` concatenation in periodic paths (`loop()`,
  `net_task`, `httpd` handlers). Use `snprintf` into fixed buffers.
- Use `JsonArena` backed by `alignas(8)` static buffers (with
  `-DARDUINOJSON_POOL_CAPACITY=64`) instead of heap-allocating `JsonDocument`s.
  Always pass a `DeserializationOption::Filter(filter)` when parsing external
  payloads like `/measures/current`.
- Bound all HTTP bodies: `AgClient` reads into a fixed static buffer (`g_body`);
  `esp_http_server` JSON endpoints enforce `kMaxBody` (`413 Payload Too Large`),
  bounded `httpd_req_recv` retries/timeouts, and `Connection: close` + returning
  `ESP_FAIL` (`finish_unread()`) on early rejections (`401`, `403`, `413`, `429`).
- Monitor heap with ESP-IDF `esp_get_free_heap_size()` and
  `esp_get_minimum_free_heap_size()` (warn below `kHeapWarnBytes`).

## Subsystems & Security
- **AirGradient local LAN API (`ag_client`):** Plain HTTP (`esp_http_client`) to
  `/measures/current` using the 4-step cascade; verify `serialno` strictly when
  `ag_serial` is set; track the `boot` counter (`ag::StaleDetector`) so 3
  unchanged polls set `ag_data_stale` and 4+ (`frozen`) escalate to failed polls.
- **mDNS coexistence:** Matter owns the primary ESP-IDF mDNS responder. Register
  `atoms3-iaq.local` as a delegate hostname (`mdns_delegate_hostname_add` /
  `mdns_service_add_for_host`); query AirGradient via `mdns_query_a` /
  `mdns_query_ptr`.
- **REST API auth (`api_auth`):** Mutating endpoints require `auth_require(req)`
  (constant-time Bearer check, per-IP lockout). `POST /api/v1/pair` checks the
  `Host`/`Origin` CSRF guard (`pairing_request_allowed`) before consuming the
  button-opened window. Tokens live only in NVS (`iaqauth`), never in
  `/config.json`, GET responses, or logs.
- **IR TX (`ir_nec_purifier`):** Use ESP-IDF RMT TX (`driver/rmt_tx.h`,
  `driver/rmt_encoder.h`) from `loop()` only (`IrNecPurifier::loop()`). Default
  NEC address word is `0xFF00` (warn on byte-swapped `0x00FF`). Send 2 frames per
  command (`100 ms` gap), enforce inter-sequence gap, apply the `600 ms` Speed-1
  wake prefix when waking from Off to speed >= 2 (if enabled), keep open-loop
  periodic refresh (`ir_refresh_mins` via `resend()` with `assume_off = true`),
  and keep stop-mark `duration1 = 1` µs so RMT does not truncate the stop pulse.
- **Future TLS / HTTPS (e.g., OTA):** Sync clock before validating certs; keep
  `setInsecure()` strictly behind `#ifdef AG_ALLOW_INSECURE_TLS`; never commit
  secrets or include them in Wokwi simulation builds.

## Structure
- Keep policy, parsing, validation, auth, LED priority, clock parsing, and IR
  planning free of hardware/Arduino dependencies (`#ifndef UNIT_TEST` around
  device code) so they compile and run in `env:native`. Pass time in as a
  parameter rather than calling `millis()` inside pure logic.
- Pins, thresholds, and intervals are named constants in headers (`sys_config.h`,
  `iaq_algorithm.h`, `ir_nec_purifier.h`, `api_auth.h`, etc.). No magic numbers.
- Represent sensor readings with explicit `*_valid` flags; never use `0` as "no
  data" or drive the purifier Off on missing/stale/frozen data.

## Logging
- Use ESP-IDF `ESP_LOGI` / `ESP_LOGW` / `ESP_LOGE` / `ESP_LOGD` with a module
  `kTag` constant (not `log_i` or `Serial` in periodic paths). Keep the periodic
  control status line on one line (`[ctl] ...`) including
  `esp_get_free_heap_size()` and `esp_get_minimum_free_heap_size()`. Never log
  tokens, passwords, or secrets.

## Verification
- After editing, `~/.platformio/penv/bin/pio run -e atoms3` must compile with no
  new warnings.
- After editing host-testable logic, `~/.platformio/penv/bin/pio test -e native`
  must pass.
