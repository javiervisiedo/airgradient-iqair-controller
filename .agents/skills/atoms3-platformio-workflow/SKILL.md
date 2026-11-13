---
name: atoms3-platformio-workflow
description: >-
  Use this skill to build, flash, upload LittleFS, or serial-monitor AtomS3 Lite
  firmware with PlatformIO (pioarduino), to run host-side unit tests (`native`),
  or to diagnose pioarduino install-hook issues, upload failures, boot loops,
  crashes, or missing serial output on the ESP32-S3.
---

# AtomS3 Lite PlatformIO Workflow

> [!NOTE]
> **Pre-hardware status:** No physical AtomS3 Lite is attached yet. Building
> (`atoms3`, `atoms3_debug`) and host tests (`native`) work locally; whole-firmware
> verification uses Wokwi (`wokwi-simulation` skill) once `diagram.json` and
> `wokwi.toml` are set up.

The helper [pio.sh](scripts/pio.sh) wraps common operations and finds both `pio`
(including `~/.platformio/penv/bin/pio`) and the AtomS3 serial port automatically.

## Toolchain & `platformio.ini` Facts

- **Platform:** **pioarduino `55.03.312-1`** (Arduino-ESP32 core 3.3.12 / ESP-IDF
  5.5.5, required for Matter). Official `espressif32` does not ship Arduino core 3.x.
- **Python requirement:** PlatformIO's penv needs **Python >= 3.10** for pioarduino.
  If `pio` is not yet installed in `~/.platformio/penv/bin/pio` or `PATH`, follow
  the `uv` + Python 3.12 + `get-platformio.py` steps in `README.md` (§1).
- **Known pioarduino install-hook bug:** Installing pioarduino can overwrite
  PlatformIO Core with `pioarduino 6.2.0.post1`, which the platform then rejects
  (*"not compatible with PlatformIO Core v6.2.0-post.1"*). Fix:
  ```bash
  ~/.platformio/penv/bin/python -m pip uninstall -y pioarduino
  ~/.platformio/penv/bin/python -m pip install --force-reinstall platformio==6.2.0
  ```
- **Environments in `platformio.ini`:**
  - `atoms3` (default): `board = m5stack-atoms3`, `partitions_8MB_matter.csv`,
    `littlefs`, `8MB` flash, `CORE_DEBUG_LEVEL=3`, `IAQ_MATTER_AIR_PURIFIER_TYPE=1`,
    `ARDUINO_USB_CDC_ON_BOOT=1`, `ARDUINO_USB_MODE=1`, `upload_speed = 921600`,
    `test_ignore = *`. Expected footprint: ~42% RAM, ~1.9 MB flash.
  - `atoms3_debug`: extends `atoms3` with `build_type = debug`, `CORE_DEBUG_LEVEL=5`.
  - `native`: Unity host tests (`UNIT_TEST`), compiles only the hardware-independent
    parts of `iaq_algorithm.cpp`, `sys_config.cpp`, `ir_nec_purifier.cpp`,
    `status_led.cpp`, `clock_sync.cpp`, `ag_client.cpp`, `api_auth.cpp`.
- The first device build downloads ~2.5 GB of toolchain/framework; run with a
  long timeout.

## Commands

| Action | Command |
| :--- | :--- |
| Host unit tests | `.agents/skills/atoms3-platformio-workflow/scripts/pio.sh test` (or `pio test -e native`) |
| Build firmware | `.agents/skills/atoms3-platformio-workflow/scripts/pio.sh build` (`PIO_ENV=atoms3_debug` for debug) |
| Flash firmware | `.agents/skills/atoms3-platformio-workflow/scripts/pio.sh upload` |
| Upload default `data/config.json` to LittleFS | `pio run -e atoms3 -t uploadfs` |
| Wipe Matter pairings, Wi-Fi & NVS | `pio run -e atoms3 -t erase` |
| Monitor (background) | `.agents/skills/atoms3-platformio-workflow/scripts/pio.sh monitor` |
| Capture N seconds of serial | `.agents/skills/atoms3-platformio-workflow/scripts/pio.sh capture 30` |

## Flashing Procedure (Once Physical Hardware Arrives)

1. Tell the user before running `upload`, `uploadfs`, or `erase`, naming the port
   and environment. (A hook in `.agents/hooks/guard_commands.py` enforces approval.)
2. Stop any running `pio device monitor` first (USB-CDC port is exclusive).
3. If upload fails (`No serial data received` / `Failed to connect`), use
   **download mode**: unplug, **hold button G41**, plug in, release after ~2 s,
   retry upload, then replug/reset to boot the new firmware.

## Troubleshooting

Read [troubleshooting.md](references/troubleshooting.md) before guessing.
