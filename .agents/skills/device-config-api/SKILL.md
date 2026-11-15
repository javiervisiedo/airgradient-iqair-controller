---
name: device-config-api
description: >-
  Use this skill when modifying, testing, or reviewing the AtomS3's local HTTP
  REST API, button-initiated Bearer token pairing, per-client lockout, config
  schema and validation, NVS + LittleFS persistence, or JsonArena static memory
  allocation.
---

# Device Configuration & REST API

**Finalized architecture (SRS v2.3 §5.2–5.3):** Local HTTP REST JSON server on
port 80 (`esp_http_server.h`, mDNS `atoms3-iaq.local`), running alongside Matter.
Wi-Fi provisioning uses Matter BLE commissioning (or `include/secrets.h` for
dev/simulation).

Implementation: `include/sys_config.h`, `src/sys_config.cpp`,
`include/api_auth.h`, `src/api_auth.cpp`, `src/matter_ha_bridge.cpp`.

## Endpoints (`src/matter_ha_bridge.cpp`)

| Method & Path | Auth | Behavior |
| :--- | :--- | :--- |
| `GET /` | Open | Read-only HTML monitoring dashboard (or JSON endpoint list when `Accept: application/json`) |
| `GET /api/v1/status` | Open | Live `StatusSnapshot` + telemetry + `api_paired` / `api_pairing_active` (never returns secrets) |
| `GET /api/v1/events` | Open | Last 24 state transitions (`boot`, `speed_change`, `mode_change`, `ag_alarm`, `ag_recovered`, `watchdog_cap`), newest-first |
| `GET /api/v1/config` | Open | Current `AppConfig` JSON (contains no secrets) |
| `PATCH /api/v1/config` | Bearer | Partial JSON patch (`config_apply_json_patch`), rejects unknown keys & invalid ranges (`422`), applies via `BridgeCmd::CONFIG_PATCHED`, debounced save |
| `POST /api/v1/command` | Bearer | `{"action":"set_speed","mode":"auto"\|"night_force"}`, `{"action":"set_speed","mode":"manual","speed":0..6}`, `{"action":"test_ir","speed":0..6}`, `{"action":"reboot"}` (`202 Accepted`) |
| `POST /api/v1/pair` | Pairing window + Host/Origin guard | Returns `{"token":"<32 hex>","token_type":"Bearer"}` once during the 60 s button window |

## Security & Robustness Invariants (`api_auth` + `matter_ha_bridge.cpp`)

1. **Physical-presence pairing (`SRS §5.3`):**
   - Holding `GPIO41` for `3–10 s` and releasing opens a `60 s` window
     (`kPairWindowMs = 60000`, Cyan heartbeat LED) with a fresh 128-bit hardware
     RNG token (`esp_fill_random`, 32 lowercase hex chars).
   - `POST /api/v1/pair` checks `auth::pairing_request_allowed()` **first**
     (`Host` must be `atoms3-iaq.local` or `device_ipv4()`, optional `:80`,
     case-insensitive; `Origin`, if present, must be `http://` + an allowed host).
     A rejected request does NOT consume the window.
   - First valid `POST /api/v1/pair` activates the new token (immediately revoking
     any prior token), closes the window, clears lockouts, and increments
     `generation_`. `loop()` retries `state_save_api_token()` every 1 s until
     the NVS write succeeds (`iaqauth` namespace, never `/config.json`, never logged).
   - Factory reset (button ≥ 10 s) calls `auth_clear()`, `state_erase_api_token()`,
     and `Matter.decommission()`.
2. **Bearer auth & per-client lockout (`ApiAuth::check`):**
   - `Authorization: Bearer <token>` (case-insensitive scheme, constant-time compare).
   - 5 bad tokens from one client IP within `60 s` lock **only that IP** out for
     `60 s` (`429 Too Many Requests` + `Retry-After`). Up to `kMaxTrackedPeers = 8`
     clients tracked; unlocked LRU is evicted before locked entries.
   - Windows and lockouts expire and clear on elapsed-time checks (`millis()` wrap safe).
3. **Request robustness:**
   - Max body `kMaxBody = 1536` bytes (`413 Payload Too Large` above that).
   - Any early rejection (`401`, `403`, `413`, `429`) sets `Connection: close` and
     returns `ESP_FAIL` from the handler (`finish_unread()`) so `esp_http_server`
     closes the socket instead of draining an attacker's slow/large body.

## Configuration Model, Validation & Persistence (`sys_config`)

- **Single model (`AppConfig`):** Validated ranges in `config_validate()`:
  - `operating_mode`: `"auto"` / `"manual"` / `"night_force"`
  - `manual_speed`: `0..6`, `manual_timeout_mins`: `0..1440`
  - `room_volume_m3`: `30.0..500.0`
  - `ag_serial`: 1..16 lowercase hex chars; `ag_target_host`: host chars (`[A-Za-z0-9.\-_:]`)
  - `poll_interval_sec`: `30..600` (default `60`)
  - `day_start_hour`, `night_start_hour`: `0..23`, must differ; `tz_posix`: non-empty
  - `ir_codes[0..6]`: valid NEC words (`high_byte == (~low_byte & 0xFF)`)
  - `ir_refresh_mins`: `0..240`
  - Note: warn on `ir_nec_address == 0x00FF` (`ir_address_looks_swapped()`).
- **Atomic PATCH + rollback:** `h_config_patch` commits non-mode fields under
  `g_cfg_mutex` and rolls back `*g_self->config()` if `post(CONFIG_PATCHED)` fails (`503`).
- **Persistence & flash wear:** `loop()` is the **only** writer of NVS/LittleFS
  (debounced `kConfigSaveDebounceMs = 5000 ms`, writes NVS `iaqcfg` + LittleFS
  `/config.json` backup only when the serialized JSON actually changed).
- **Zero-heap JSON (`JsonArena`):** `sys_config.cpp` uses a static 6 KB arena
  (`g_cfg_json_buf` guarded by `g_buf_mutex`), and `matter_ha_bridge.cpp` uses a
  static 8 KB `g_http_arena` in the single-threaded `httpd` task. Keep
  `-DARDUINOJSON_POOL_CAPACITY=64` in `platformio.ini`.

## Verification

- **Host tests (`pio test -e native`):** `test_config_patch_validation` and all
  `test_TC08_*` / `test_review_*` cases in `test/test_algorithm/test_main.cpp`.
- **On-device / LAN `curl`:** Test `401` before pairing, button 3–10 s hold →
  `POST /api/v1/pair` (check Host/Origin rejection), `PATCH /api/v1/config` (`422`
  on bad key/range, `200` on valid), `429` after 5 bad tokens, and reboot persistence.
- **Wokwi:** Incoming HTTP to port 80 requires Wokwi's paid Private Gateway
  (`http://localhost:9080/`). On the free plan, cover HTTP/auth logic in `native` tests.
