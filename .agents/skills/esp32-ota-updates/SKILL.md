---
name: esp32-ota-updates
description: >-
  Use this skill when implementing, testing, or reviewing over-the-air (OTA)
  firmware updates on the AtomS3 Lite: using the dual-OTA partition layout in
  partitions_8MB_matter.csv, authenticated HTTP OTA upload or Matter OTA,
  rollback verification, and avoiding WDT/heap/flash-wear failures on ESP32-S3
  without PSRAM.
---

# ESP32-S3 OTA Firmware Updates

**Status:** Not yet implemented (`README.md` Known limitations), but the flash
layout (`partitions_8MB_matter.csv`) already reserves two 3.5 MB OTA slots.

## Partition Budget (`partitions_8MB_matter.csv`)

| Partition | Offset | Size | OTA Behavior |
| :--- | :--- | :--- | :--- |
| `nvs` | `0x9000` | 64 KB | **Preserved** across OTA (Matter fabrics, Wi-Fi, `iaqcfg`, `iaqauth` REST token) |
| `otadata` | `0x19000` | 8 KB | Tracks active boot slot (`ota_0` / `ota_1`) and pending-verify state |
| `app0` | `0x20000` | 3.50 MB | `ota_0` (current app binary is ~1.9 MB) |
| `app1` | `0x3A0000` | 3.50 MB | `ota_1` |
| `fctry` | `0x720000` | 24 KB | **Preserved** (Matter factory data) |
| `littlefs` | `0x726000` | 872 KB | **Preserved** on app OTA (`/config.json` backup) |

## Design Constraints (ESP32-S3FN8, 512 KB SRAM, No PSRAM)

1. **Authentication is mandatory.** Any local HTTP OTA endpoint (e.g.,
   `POST /api/v1/ota`) MUST pass `MatterHaBridge::auth_require(req)` before reading
   any payload, and reject unauthenticated requests with `401`/`429` +
   `Connection: close` (`ESP_FAIL`). Do NOT add unauthenticated `ArduinoOTA` on
   port 3232 without explicit auth.
2. **Stream directly to flash via `esp_ota_ops.h` (`esp_ota_begin` /
   `esp_ota_write` / `esp_ota_end` / `esp_ota_set_boot_partition`).**
   - Never buffer the ~1.9 MB image in RAM. Read and flash in 4 KB stack/static
     chunks.
   - Note `esp_http_server`'s default `kMaxBody` (`1536` B in
     `matter_ha_bridge.cpp`) is for JSON only; an OTA handler must stream
     `req->content_len` up to the partition size (`0x380000` = 3,670,016 bytes)
     and reject larger images immediately (`413`).
   - Validate the image header (`ESP_IMAGE_HEADER_MAGIC = 0xE9`) on the first
     chunk before erasing the entire inactive OTA slot.
3. **Watchdog & task coordination:**
   - Flash sector erases (`esp_ota_begin` with `OTA_SIZE_UNKNOWN` or incremental
     erases) can stall SPI flash access. Pass the exact `content_len` to
     `esp_ota_begin` or use `OTA_WITH_SEQUENTIAL_WRITES` so erases happen
     incrementally per sector instead of one multi-second erase before the first
     chunk.
   - `kHttpMaxUriHandlers = 8` in `src/matter_ha_bridge.cpp` currently registers
     6 routes, leaving room for `/api/v1/ota` (update `h_root` discovery JSON too).
   - `httpd` is NOT registered on the Task Watchdog; do **not** call
     `esp_task_wdt_reset()` from an HTTP handler (`ESP_ERR_NOT_FOUND`). Instead,
     use `OTA_WITH_SEQUENTIAL_WRITES` and yield (`vTaskDelay(1)`) between 4 KB
     chunk writes so `loop()` and `net_task` continue feeding their 15 s TWDTs,
     and bound `httpd_req_recv` timeouts so a stalled uploader aborts cleanly
     with `esp_ota_abort()`.
   - Pause/drain pending config saves and wait until `!g_ir.busy()` before
     calling `esp_restart()`.
4. **Rollback protection (critical for unattended operation):**
   - Enable bootloader app rollback in sdkconfig/build if supported, and verify
     `esp_ota_mark_app_valid_cancel_rollback()` is called only after critical
     subsystems initialize (`config_load`, `g_ir.begin`, `g_bridge.begin`, and
     Wi-Fi/HTTP up if previously commissioned).
5. **Memory safety (HTTPS pull OTA vs HTTP push OTA):**
   - Local HTTP push (`POST /api/v1/ota`) adds almost zero heap overhead (reuses
     the existing `esp_http_server`).
   - If pull OTA over HTTPS is ever added, never run a TLS client concurrently
     with heavy heap allocations; verify `esp_get_free_heap_size()` stays well
     above `120 KB` (`kHeapWarnBytes`).
   - If Matter OTA Requestor (`ESP-Matter`) is used instead of or alongside HTTP
     OTA, verify flash size still fits comfortably inside `3.50 MB`.

## Verification Checklist

- Confirm `pio run -e atoms3 -t size` remains safely below `0x380000` (3,670,016 B).
- Verify unauthenticated `POST /api/v1/ota` returns `401` and closes the socket.
- Verify uploading a truncated or non-ESP32 binary fails cleanly (`esp_ota_abort`),
  leaves the running slot active, and does not corrupt `nvs` / `iaqauth` / Matter
  fabrics.
- Verify a valid OTA upload boots the new slot while preserving Matter pairings,
  REST API token, and `/api/v1/config` settings.
