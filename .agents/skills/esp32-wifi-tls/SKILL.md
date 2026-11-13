---
name: esp32-wifi-tls
description: >-
  Use this skill when designing or debugging connectivity on the AtomS3 Lite:
  Wi-Fi connect/reconnect behaviour, Matter mDNS delegate coexistence
  (atoms3-iaq.local), 4-tier clock synchronization (DHCP Opt 42, SNTP, Matter/HTTP
  Date, RTC), FreeRTOS task architecture, 512 KB SRAM heap budget, or optional
  future TLS (e.g., HTTPS OTA).
---

# ESP32-S3 WiFi, mDNS, Clock Sync & Heap Architecture

> [!NOTE]
> **Architecture update (2026-11-09, SRS v2.2):** The firmware polls the
> AirGradient monitor over **local plain HTTP only** (`AgClient`, port 80). There
> is currently **no TLS (`WiFiClientSecure`) or CA bundle** in the firmware. The
> TLS script/reference in this skill (`make_ca_header.sh`, `tls_certificates.md`)
> applies only if HTTPS OTA or cloud fallback is ever added later.

## Wi-Fi & Threading Topology (`src/main.cpp`)

- **2.4 GHz only:** ESP32-S3 has no 5 GHz radio; avoid WPA3-only SSIDs.
- **Provisioning:** Matter BLE commissions Wi-Fi into NVS on real hardware. For
  dev/simulation without BLE, create gitignored `include/secrets.h` with
  `WIFI_SSID` and `WIFI_PASSWORD`.
- **Tasks & Watchdog:**
  - `loop()` (Core 1): non-blocking (~200 Hz, `delay(5)`), handles button,
    `g_cmd_q`, `g_poll_q`, `ControlCore`, `IrNecPurifier`, `StatusLed`, NVS saves.
  - `net_task` (Core 0, 8 KB stack): runs `ClockSync::loop()` and blocking
    `AgClient::poll()` (bounded timeouts via `esp_http_client`), posts `PollResult`
    to `g_poll_q`. Communicates with `WiFi.onEvent()` via atomics (`g_ev_got_ip`,
    `g_ev_disconnected`).
  - Both `loop()` and `net_task` are registered on the **15 s Task Watchdog**
    (`kWdtTimeoutMs = 15000`, `trigger_panic = true`). Reset TWDT between blocking
    discovery/HTTP steps.
  - If Wi-Fi never connects within `120 s` after boot (`kNoNetworkFailAfterMs`),
    `net_task` starts emitting failed polls so the watchdog/alarm covers
    "never connected".

## mDNS Coexistence with Matter (`matter_ha_bridge.cpp`, `ag_client.cpp`)

- The Matter stack initializes and owns the primary ESP-IDF mDNS responder.
- **Never overwrite the primary mDNS hostname.** Register `atoms3-iaq.local` and
  `_http._tcp:80` as a **delegate hostname** via `mdns_delegate_hostname_add()` /
  `mdns_service_add_for_host()` in `MatterHaBridge::update_mdns()` on `GOT_IP`.
- `AgClient` queries the shared ESP-IDF mDNS API (`mdns_query_a`, `mdns_query_ptr`).

## 4-Tier Resilient Clock Synchronization (`clock_sync.h`, `clock_sync.cpp`)

1. **Tier 1 — DHCP Option 42:** `esp_sntp_servermode_dhcp(true)` MUST be called
   in `ClockSync::begin()` **after `Network.begin()` and before DHCP/Wi-Fi connect**.
2. **Tier 2 — Multi-Server SNTP:** On `GOT_IP`, configure SNTP with
   `[DHCP Option 42 server or gateway IP], pool.ntp.org, time.google.com`.
   First sync uses `SNTP_SYNC_MODE_IMMED`, then switches to
   `SNTP_SYNC_MODE_SMOOTH` (`adjtime` every 3 h). Local timezone is applied via
   `setenv("TZ", tz_posix, 1); tzset();` (default `"SGT-8"`).
3. **Tier 3 — Matter / HTTP `Date` Fallback (after 30 s):** If SNTP has not
   synced within `30 s` (`kSntpGraceMs`), try `matter_utc()` (currently inactive
   until Time Synchronization cluster `0x0038` is added), then `HTTP HEAD /` to
   `http://<gateway_ip>/` and `http://google.com/`, parsing RFC 7231 `Date:`
   (`clk::parse_http_date`). Retries every 5 min while unsynced.
4. **Tier 4 — RTC Retention & Day Fallback:** Warm reboots retain RTC time
   (`time(nullptr) >= 1700000000` → `rtc_retained`). Cold boot with no valid time
   sets `unsynced_fallback_day` and defaults to **Day Mode** unless `NIGHT_FORCE`
   is active (`iaq::resolve_is_night`).

## 512 KB Internal SRAM Budget (No PSRAM)

- Matter + Wi-Fi + IPv4/IPv6 + `esp_http_server` + `esp_http_client` share
  512 KB internal SRAM.
- `Matter.setBLEMemoryReleaseEnabled(true)` reclaims ~65 KB once commissioned.
- Use static buffers / `JsonArena` and `esp_http_client` / `esp_http_server`
  instead of dynamic `String` concatenation.
- Monitor `esp_get_free_heap_size()` and `esp_get_minimum_free_heap_size()`;
  `main.cpp` warns below `kHeapWarnBytes = 120 * 1024` (SRS §6.2).

## TLS Reference (Optional / Future Use Only)

If TLS is added later (e.g., HTTPS OTA), remember ESP32 boots at epoch 1970 (so
sync clock first), never enable `setInsecure()` outside `#ifdef AG_ALLOW_INSECURE_TLS`,
and generate `ISRG Root X1 + X2` with `.agents/skills/esp32-wifi-tls/scripts/make_ca_header.sh`
(see [tls_certificates.md](references/tls_certificates.md)).
