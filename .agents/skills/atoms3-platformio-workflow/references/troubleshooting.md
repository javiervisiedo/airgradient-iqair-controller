# AtomS3 Lite Troubleshooting

| Symptom | Likely cause | Fix |
| :--- | :--- | :--- |
| `IncompatiblePlatform ... PlatformIO Core v6.2.0-post.1` | pioarduino install hook replaced PlatformIO | Run `~/.platformio/penv/bin/python -m pip uninstall -y pioarduino && ~/.platformio/penv/bin/python -m pip install --force-reinstall platformio==6.2.0`. |
| Upload: `Failed to connect to ESP32-S3: No serial data received` | Running firmware owns USB-CDC, or crashed USB stack | Download mode: unplug, hold G41 button, plug in, release after ~2 s. Re-upload, then replug/reset. |
| Upload: `Resource busy` / `could not open port` | `pio device monitor` or another process holds the port | Stop the monitor task, then retry. |
| No port in `/dev/cu.usbmodem*` or `/dev/ttyACM*` | Charge-only USB-C cable, or hub issue | Use a data-capable USB-C cable plugged directly into the host. |
| Flash OK but serial is silent | Missing `ARDUINO_USB_CDC_ON_BOOT=1` / `ARDUINO_USB_MODE=1`, or early log before enumeration | `setup()` waits `1500 ms` after `Serial.begin(115200)`; verify both flags in `platformio.ini`. |
| `ESP_LOGI` / `log_i` missing | `CORE_DEBUG_LEVEL` < 3 | Use `atoms3` (`CORE_DEBUG_LEVEL=3`) or `atoms3_debug` (`CORE_DEBUG_LEVEL=5`). |
| Boot loop with `rst:0xc (RTC_SW_CPU_RST)` or `Stack canary watchpoint triggered` | Null pointer, stack overflow (`net_task` 8 KB / `httpd` 6 KB / `loop`), or heap exhaustion | Decode backtrace via `esp32_exception_decoder`; check heap (`kHeapWarnBytes = 120 KB`). |
| `Brownout detector was triggered` | Weak USB port during WiFi TX bursts | Use a better 5 V supply or shorter USB-C cable. |
| `Task watchdog got triggered` (15 s TWDT) | Blocking call in `loop()` or `net_task` without `esp_task_wdt_reset()` | Keep `loop()` non-blocking; ensure `net_task` resets TWDT between discovery/HTTP steps. |
| `low heap` warning (`< 120 KB`) or Matter/HTTP alloc failure | Fragmentation or BLE RAM not yet released | Check `min_free_heap_bytes` in `/api/v1/status`; avoid `String` in periodic paths; keep `JsonArena` static. |
| AirGradient amber/red (`ag_status != online`) | mDNS blocked, wrong `ag_serial`, or frozen `boot` counter | Check `ag_serial` (`serialno` must match), `ag_data_stale`, or set `ag_target_host` to the monitor's IP. |
| IR does nothing | Wrong `ir_nec_address` (check `0xFF00` vs `0x00FF`), out of range (1–2 m LOS), or bad aim | Test with `POST /api/v1/command {"action":"test_ir","speed":3}` + phone front camera. Check `ir_nec_address` warning at boot. |
| Host (`native`) tests fail to compile/link | Hardware/Arduino symbols leaked outside `#ifndef UNIT_TEST` | Keep `Arduino.h`, `WiFi.h`, `driver/*`, `Preferences.h`, `esp_*` behind `#ifndef UNIT_TEST`. |
| WiFi never connects | 5 GHz-only SSID, or WPA3-only | Use a 2.4 GHz SSID with WPA2/WPA3 mixed mode. |
| Future TLS (HTTPS OTA/cloud) `-0x2700` / `-0x7780` | Unsynced clock, wrong CA, or SNI mismatch | Sync RTC before TLS; verify CA bundle via `esp32-wifi-tls` `make_ca_header.sh`. |
