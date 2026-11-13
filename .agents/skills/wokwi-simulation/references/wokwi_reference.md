# Wokwi Reference for the AtomS3 Lite Stand-in

All facts here were checked against docs.wokwi.com and the wokwi-boards / wokwi-cli repos (2026-11).
`wokwi.toml`, `diagram.json`, and `[env:atoms3_sim]` are configured in the repo root.
This page documents what they contain.

## Hardware mapping (AtomS3 Lite to diagram.json)

| AtomS3 Lite | Wokwi part / setting | Notes |
| :--- | :--- | :--- |
| ESP32-S3FN8 | `board-esp32-s3-devkitc-1`, attrs `"flashSize": "8"` | No AtomS3 board exists in Wokwi |
| USB-CDC console | attr `"serialInterface": "USB_SERIAL_JTAG"` | Matches `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1` |
| IR LED on GPIO4 | `esp:4` → `wokwi-logic-analyzer` `D0` (plus an optional `wokwi-led` A pin, C to GND) | The logic analyzer exports VCD for decoding |
| WS2812 on GPIO35 | `wokwi-neopixel`: `DIN`←`esp:35`, `VDD`←`esp:3V3.1`, `VSS`←`esp:GND.1` | |
| Button on GPIO41 (active low, pull-up) | `wokwi-pushbutton` `1.l`←`esp:41`, `2.l`←GND; `wokwi-resistor` 10 kΩ from `esp:41` to `esp:3V3.2` | `main.cpp` uses `INPUT_PULLUP`, and the external 10 kΩ pull-up is good practice in `diagram.json` |
| No PSRAM | (nothing) | Firmware built for `m5stack-atoms3` doesn't enable PSRAM |

Scenario control for the button: `set-control` with `part-id: <button id>`, `name: pressed`, `value: 1|0`.

Example skeleton (illustrative. Copy the part layout from `wokwi-cli init` or wokwi.com):

```json
{
  "version": 1,
  "parts": [
    { "type": "board-esp32-s3-devkitc-1", "id": "esp", "attrs": { "flashSize": "8", "serialInterface": "USB_SERIAL_JTAG" } },
    { "type": "wokwi-logic-analyzer", "id": "la1", "attrs": { "channelNames": "IR" } },
    { "type": "wokwi-neopixel", "id": "rgb1" },
    { "type": "wokwi-pushbutton", "id": "btn1" },
    { "type": "wokwi-resistor", "id": "r1", "attrs": { "value": "10000" } }
  ],
  "connections": [
    ["esp:4", "la1:D0", "green", []], ["esp:GND.2", "la1:GND", "black", []],
    ["esp:35", "rgb1:DIN", "green", []], ["esp:3V3.1", "rgb1:VDD", "red", []], ["esp:GND.1", "rgb1:VSS", "black", []],
    ["esp:41", "btn1:1.l", "blue", []], ["esp:GND.3", "btn1:2.l", "black", []],
    ["esp:41", "r1:1", "blue", []], ["esp:3V3.2", "r1:2", "red", []]
  ]
}
```

Validate with `wokwi-cli lint` (checks part types and pin names). With
`serialInterface: USB_SERIAL_JTAG` the console is the USB serial. If the serial
log stays empty, check this attribute and the `ARDUINO_USB_*` build flags first.

## wokwi.toml

```toml
[wokwi]
version = 1
firmware = '.pio/build/atoms3_sim/firmware.factory.bin'
elf = '.pio/build/atoms3_sim/firmware.elf'
# gdbServerPort = 3333      # optional: start paused, attach xtensa-esp32s3-elf-gdb
```

**Full image required.** PlatformIO's `firmware.bin` is only the application. Wokwi
would add its own bootloader, a default partition table, and 4 MB of flash. This
project uses **`partitions_8MB_matter.csv`** (dual 3.5 MB OTA + `nvs` + `fctry` +
`littlefs`), so `wokwi.toml` uses pioarduino's merged `firmware.factory.bin`
(bootloader at `0x0`, partitions at `0x8000`, `boot_app0` at `0xe000`, app at
`0x20000`) and `diagram.json` sets `flashSize` to `"8"`.

## Networking inside the simulation

| Fact | Value |
| :--- | :--- |
| SSID | `Wokwi-GUEST`, open (no password), channel 6. Pass channel 6 to `WiFi.begin` to skip scanning (~4 s) |
| Custom SSID/WPA2 | Only with Hobby+ / Pro (`wokwi-wifi-ap` part) |
| Internet | Through the Wokwi IoT Gateway. Outgoing TCP and UDP (DNS, NTP, HTTPS work). No ICMP/ping |
| Public Gateway (free) | Cloud-hosted, **traffic monitored**, device IP 10.10.0.2, no access to your LAN or localhost |
| Private Gateway (paid) | Runs on your machine, unmonitored, device IP 10.13.37.2. Reaches host services at `host.wokwi.internal` |
| Incoming connections | **Not supported** on the Public Gateway. Private Gateway: `http://localhost:9080/` goes to device port 80; other ports with gateway `--forward <local>:10.13.37.2:<port>` |
| MQTT | Outgoing to internet brokers via either gateway. A LAN or HA broker needs the Private Gateway (`host.wokwi.internal` or a LAN IP) |
| mDNS | Not documented. Assume the local AirGradient API (`airgradient_<serial>.local`) isn't reachable |

**Consequence for Wi-Fi, Matter & AirGradient in simulation:**
1. **Wi-Fi without BLE:** Wokwi has no Bluetooth, so Matter BLE commissioning
   cannot run. Use gitignored `include/secrets.h` (`WIFI_SSID="Wokwi-GUEST"`,
   `WIFI_PASSWORD=""`) or define `-DWIFI_SSID=\"Wokwi-GUEST\" -DWIFI_PASSWORD=\"\"`
   in `[env:atoms3_sim]` `build_flags` (public simulation SSID, not a secret) so
   `setup()` connects to `Wokwi-GUEST` (channel 6).
2. **Firmware-side serial injection** (free, deterministic): add a `-DWOKWI_SIM`
   serial command parser to inject `pm25`/`tvoc`/`nox` readings via scenario
   `write-serial`. Tests the control loop, RMT IR waveforms, LED, and button
   without needing the Private Gateway.
3. **Mock server via the Private Gateway** (paid plan): run
   `.agents/skills/airgradient-api/scripts/mock_airgradient.py` on port 8080 and
   set `ag_target_host` to `host.wokwi.internal:8080` (with matching `ag_serial`:
   `mock00000001`). Covers `AgClient` HTTP, stale/frozen `boot`, and error paths.
   (Whether `wokwi-cli` cloud simulation supports the Private Gateway should be
   probed before relying on it.)
4. **Never use real home Wi-Fi credentials** in a simulation build when using
   the monitored Public Gateway.

## wokwi-cli essentials (v0.29.1)

| Option | Use |
| :--- | :--- |
| `--timeout <ms>` | Simulated-time limit (default 30000). Exit code 42 on timeout |
| `--expect-text "<s>"` / `--fail-text "<s>"` | Pass or fail on serial output |
| `--scenario <file.yaml>` | Automation steps (alpha API) |
| `--serial-log-file <path>` | Save the console |
| `--vcd-file <path>` | Export logic-analyzer data |
| `--interactive` | stdin goes to the simulated serial port |
| `--elf`, `--diagram-file` | Override wokwi.toml paths |
| `-g <port>` | GDB server, start paused (pair with `--timeout 0`) |
| `wokwi-cli lint` | Validate diagram.json |
| `wokwi-cli init` | Generate wokwi.toml + diagram.json (asks questions; interactive) |
| `wokwi-cli mcp` | Experimental MCP server for AI agents (needs `WOKWI_CLI_TOKEN`) |

Heap monitoring (`--heap-*`) is documented for ESP-IDF firmware. Don't rely on it
for Arduino builds. Log `esp_get_free_heap_size()` / `esp_get_minimum_free_heap_size()` instead (as `main.cpp` does).

## Scenario steps (alpha)

```yaml
name: 'button override'
version: 1
steps:
  - wait-serial: 'setup done'                              # substring match on console
  - set-control: { part-id: btn1, name: pressed, value: 1 }
  - delay: 150ms                                         # units required
  - set-control: { part-id: btn1, name: pressed, value: 0 }
  - expect-pin: { part-id: esp, name: 4, value: 0 }      # pin level check
  - write-serial: 'inject pm25=42.0'                     # only if -DWOKWI_SIM adds a serial command
  - wait-serial: 'cmd=4'
```

## Limits and quota

- Free: 50 simulated minutes per rolling 30 days. Hobby 200, Hobby+ 500, Pro 2000.
- Wall-clock cap per session: 5 min (Free/Hobby), 10 min (Hobby+), 50 min (Pro).
- Each run counts its simulated time. Keep `--timeout` tight and use `--expect-text` so runs end early.
