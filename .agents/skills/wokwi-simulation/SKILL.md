---
name: wokwi-simulation
description: >-
  Use this skill to run, script, or debug firmware simulations while no physical
  AtomS3 Lite is available: Wokwi (wokwi-cli) setup and token, wokwi.toml and
  diagram.json requirements for the ESP32-S3 stand-in board with
  partitions_8MB_matter.csv, simulated Wi-Fi (Wokwi-GUEST) and no-BLE Matter
  constraints, automation scenarios (button presses, serial checks), capturing
  and decoding the RMT NEC IR waveform on GPIO4, and mocking the local
  AirGradient API.
---

# Firmware Simulation (Wokwi)

There's no physical device yet. **Simulation replaces flashing.** The platform
decision and its reasoning are in [simulator_selection.md](references/simulator_selection.md):
Wokwi for whole-firmware runs, plus PlatformIO `native` unit tests for pure logic.

## Pick the cheapest test that answers the question

| Question | Use |
| :--- | :--- |
| Is the control policy, watchdog, mode, config, auth, or NEC encoding right? | Host unit tests (`pio test -e native`). Free and fast |
| Does it boot, connect to Wi-Fi, sync time, poll/inject AirGradient, drive LED/button, and emit valid 38 kHz NEC RMT waveforms on GPIO4? | Wokwi |
| Do Apple/Google Home Matter BLE commissioning or the real IQAir purifier react? | Only real hardware (Wokwi has no Bluetooth) |

Wokwi minutes are limited (free plan: 50 simulated min per rolling 30 days). Check
`scripts/sim.sh usage` before long or repeated runs.

## One-time setup (needs user's Wokwi account)

1. Install the CLI: `curl -L https://wokwi.com/ci/install.sh | sh`
   (or download from github.com/wokwi/wokwi-cli/releases).
2. The user creates a token at https://wokwi.com/dashboard/ci and puts
   `WOKWI_CLI_TOKEN=wok_...` in the gitignored `.env` (or exports it).
   **Never ask the user to paste the token into chat, and never echo it.**
3. `scripts/sim.sh check` must report `ok` for the CLI and the token.

## Project files for Wokwi (`README.md`)

`diagram.json`, `wokwi.toml`, `[env:atoms3_sim]` (in `platformio.ini`), `-DWOKWI_SIM`
serial injection (`src/sim_inject.cpp`), and `sim/scenarios/tc03_wake_ir.yaml` are
configured in the repo. Details: [wokwi_reference.md](references/wokwi_reference.md):

- `diagram.json`: `board-esp32-s3-devkitc-1` (`flashSize: "8"`,
  `serialInterface: "USB_SERIAL_JTAG"`), a `wokwi-logic-analyzer` on `GPIO4`, a
  `wokwi-neopixel` on `GPIO35`, and a `wokwi-pushbutton` + 10 kΩ pull-up on `GPIO41`.
- `wokwi.toml`: points at `.pio/build/atoms3_sim/firmware.factory.bin` (merged full
  8 MB image built by pioarduino with bootloader + `partitions_8MB_matter.csv` +
  `boot_app0` + app) and `.pio/build/atoms3_sim/firmware.elf`.
- Simulation Wi-Fi & data path:
  - Wokwi has **no BLE**, so Matter BLE commissioning cannot run. `[env:atoms3_sim]`
    defines `WIFI_SSID="Wokwi-GUEST"` and `WIFI_PASSWORD=""` (public simulation
    SSID; `secrets.h` is excluded when `WOKWI_SIM` is set).
  - AirGradient uses **local HTTP (`/measures/current`)**, and mDNS
    `airgradient_<serial>.local` doesn't cross the Wokwi gateway. Use serial
    injection (`inject pm25=<v> [tvoc=<v>] [nox=<v>] [co2=<v>] [night=0|1]` or
    `inject fail [night=0|1]`) on the free plan, or point `ag_target_host` at
    `host.wokwi.internal:8080` running `mock_airgradient.py` via the paid Private
    Gateway.

## Running

Use [sim.sh](scripts/sim.sh). Outputs go to `sim-out/`.

```bash
S=.agents/skills/wokwi-simulation/scripts
pio run -e atoms3_sim                      # always rebuild first; Wokwi doesn't compile
$S/sim.sh lint                             # after any diagram.json change
$S/sim.sh run --timeout 20000 --expect "setup done"
$S/sim.sh run --timeout 60000 --scenario sim/scenarios/<name>.yaml
$S/sim.sh ir --expect speed_1,speed_1,speed_4,speed_4  # 2 frames per step (wake + target)
$S/sim.sh usage
```

Exit `0` = pass, `42` = timeout (`--expect` never matched), anything else =
failure or `--fail-text` match. Always read `sim-out/serial.log` before drawing
conclusions.

## Verifying IR without a purifier

Whenever the firmware sends an IR command in simulation, run `sim.sh ir`. The
decoder ([vcd_nec_decode.py](scripts/vcd_nec_decode.py)) demodulates the 38 kHz
carrier and prints `addr`, `cmd`, `valid` (inverse bytes), `carrier_khz`, and
the GC label. Pass criteria: protocol NEC, `addr 0x00`, expected command
sequence (including `speed_1` wake prefix on `0 -> S >= 2`, 2 frames per step,
`100 ms` frame gap, `600 ms` wake delay), `valid: true`, `carrier_khz` ~38 kHz.
`vcd_nec_decode.py --selftest` checks the decoder itself.

## Mock AirGradient

[mock_airgradient.py](../airgradient-api/scripts/mock_airgradient.py) serves the
local `/measures/current` API with advancing `boot` counter, `frozen` (TC-09),
`null_pm`, `malformed`, `slow`, `--pm25-series`, and runtime `/_mock/set`.
Inside Wokwi it requires the paid Private Gateway (`host.wokwi.internal`).

```bash
python3 .agents/skills/airgradient-api/scripts/mock_airgradient.py --port 8080 --pm25-series 5,12,40,60,20,5
```

## Gotchas

- Rebuild before every run. Stale firmware is the most common false result.
- `wokwi-cli init` is interactive. Don't run it unattended; write the files instead.
- Simulated CPU speed is capped (~8 MHz) while peripheral timers run in real
  simulated time, so Matter/Wi-Fi boot takes more simulated seconds. Budget
  `--timeout` accordingly.
- Wokwi's Public Gateway is monitored: never compile real home Wi-Fi credentials
  into a simulation build.
- Incoming connections to the REST API (`:80`) require the paid Private Gateway
  (`http://localhost:9080/`). On the free plan, test HTTP/auth via `pio test -e native`.
- Wokwi results don't replace physical hardware bring-up (IR range, wake from
  standby, Apple/Google Matter BLE commissioning).
