# Simulator Selection (decided 2026-11-06)

## Requirements from this project

| Need | Why |
| :--- | :--- |
| ESP32-S3 core running real Arduino-ESP32 firmware | Same binary we will flash later |
| WiFi + TCP/UDP + DNS, NTP, local HTTP | AirGradient local LAN API, SNTP / HTTP Date clock sync, REST server |
| GPIO output observable at µs resolution on GPIO4 | Verify the 38 kHz NEC RMT IR waveform |
| RMT peripheral | `IrNecPurifier` uses ESP-IDF `driver/rmt_tx.h` (38 kHz / 33% duty) |
| WS2812 on GPIO35, button on GPIO41 | Status LED and button UX |
| USB-Serial-JTAG console | The AtomS3 logs over native USB |
| Headless, scriptable, exit codes | So the agent can run it with `run_command` |

## Candidates

| Platform | ESP32-S3 | WiFi | LEDC/RMT | GPIO matrix | USB serial | Headless | Verdict |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Wokwi** (CLI + VS Code) | ✅ (ESP32-S3-DevKitC-1) | ✅ virtual AP + internet gateway | ✅ / ✅ | ✅ | ✅ `USB_SERIAL_JTAG` | ✅ `wokwi-cli` | **Chosen for firmware simulation** |
| Espressif QEMU | ✅ | ❌ (OpenCores Ethernet only) | ❌ / ❌ (LEDC only on ESP32) | ❌ | ❌ | ✅ | Rejected: no WiFi, IR or GPIO |
| PlatformIO `native` + Unity | n/a (host) | n/a | n/a | n/a | n/a | ✅ | **Chosen for logic tests.** Complements Wokwi |
| Renode | – | – | – | – | – | ✅ | Not evaluated in depth. ESP32-S3 + WiFi support not confirmed |

Sources: Wokwi ESP32 peripheral table (docs.wokwi.com/guides/esp32), Wokwi WiFi guide,
wokwi-cli README (v0.29.1, 2026-10-23), Espressif QEMU feature table
(espressif/esp-toolchain-docs `qemu/README.md`), wokwi-boards `esp32-s3-devkitc-1/board.json`.

## Decision: two layers

1. **Host unit tests** (PlatformIO `native`, Unity): free and unlimited. They cover all pure
   decision logic (AQI, hysteresis, the target-to-IR mapping, the JSON-to-reading parsing if kept portable).
2. **Wokwi** for whole-firmware runs: boot, WiFi/NTP/TLS, the HTTP client against a
   mock or real server, the IR waveform on GPIO4, the LED, and the button. The agent
   drives it through `wokwi-cli`.

## Known compromises

- **No AtomS3 Lite board in Wokwi.** Stand-in: `board-esp32-s3-devkitc-1` (same
  ESP32-S3 chip; GPIO4, 35 and 41 are exposed). Recreate the AtomS3's pull-up on the
  button and the WS2812 in the diagram.
- **No purifier model.** The simulation proves the *waveform* (protocol, address,
  command, carrier). It doesn't prove the GC MultiGas reacts. That still needs hardware.
- **Cost/quota:** the free plan gives 50 simulated CI minutes per rolling 30 days, and each
  session is capped at 5 min wall-clock. Hobby (€5.6/mo) adds the Private IoT Gateway.
  Hobby+ (€8.1/mo) adds Wokwi for VS Code and custom WiFi APs. Prices are from wokwi.com/pricing, 2026-11.
- **The Public Gateway is monitored by Wokwi.** Never send the real AirGradient token
  through it (see the secrets rule).
- **CPU speed:** Wokwi caps the simulated CPU at about 8 MHz instruction speed by default.
  Peripheral timing stays accurate, but heavy CPU work (TLS handshakes) takes longer in
  simulated time.
