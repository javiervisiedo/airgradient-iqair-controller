---
name: purifier-ir-control
description: >-
  Use this skill when designing, writing, or debugging anything that sends IR
  commands from the AtomS3 Lite (GPIO4) to the IQAir GC MultiGas: ESP-IDF RMT
  TX NEC driver, command pacing, wake-from-off prefix, open-loop assumed state,
  periodic idempotent refresh, test_ir re-assert, and hardware bring-up.
---

# Purifier IR Control

Verified code table: `.agents/skills/ir-code-capture/references/iqair_gc_multigas.md`.
Implementation: `include/ir_nec_purifier.h`, `src/ir_nec_purifier.cpp`, invoked
from `src/main.cpp`.

## Driver & Frame Format (ESP-IDF RMT TX, Non-Blocking)

- **Peripheral:** ESP32-S3 **RMT TX** (`driver/rmt_tx.h`, `driver/rmt_encoder.h`)
  on `GPIO4` (`pins::kIrTx`), 1 MHz resolution, 38 kHz / 33% duty carrier,
  active-high, `mem_block_symbols = 64`. Does **NOT** use `IRremote.hpp`.
- **Stop-mark RMT quirk:** RMT terminates if `duration1 == 0`, so the final stop
  mark (`sym[33]`) uses `duration1 = 1` µs (0-level) in `IrNecPurifier::start_frame()`.
- **Non-blocking state machine:** `IrNecPurifier::loop(now_ms)` runs
  `IDLE -> WAIT_DELAY -> SENDING -> WAIT_GAP` using `rmt_tx_wait_all_done(..., 0)`.
  Callbacks (Matter/HTTP) never call `g_ir` directly — they post a `BridgeCmd` to
  `loop()`.
- **32-bit NEC frame (`ir::nec_raw32`):** `(cmd_word << 16) | addr_word`, sent
  **LSB-first** (`9000 µs` mark + `4500 µs` space + 32 data bits + `560 µs` stop).
  - Default `ir_nec_address`: **`0xFF00`** (address `0x00`, inverse `0xFF`).
    Warn if `0x00FF` is configured (`ir_address_looks_swapped()`), as that sends
    address `0xFF` and the purifier will ignore every command.
  - Default `ir_codes[0..6]`: `{0xFF00, 0xE817, 0xFC03, 0xFD02, 0xFA05, 0xFB04, 0xE916}`.
  - Never expose or send `timer*` codes from control logic.

## Sequencing, Wake Prefix & Pacing (`ir::plan_transition`)

- **Repeated frames:** Every command word transmits **`kFramesPerCommand = 2`**
  identical frames separated by `kFrameGapMs = 100 ms`.
- **Inter-sequence gap:** Consecutive requests wait at least
  `kInterSequenceGapMs = 100 ms` after the last frame ends (`IDLE` guard).
- **Newest request wins:** If a new `request()` arrives mid-sequence, it replaces
  any pending request and starts after the current step's frames finish (frames
  are never cut mid-transmission).
- **Wake-from-off (`wake_require_speed1_prefix`, default `true`):** When
  transitioning from `0` (Off, or unknown `sent_speed_ < 0` at boot) to
  `speed >= 2`, `plan_transition` sends **Speed 1 (`0xE817`)** first, waits
  **`kWakeDelayMs = 600 ms`**, then sends the target speed.
- **Open-loop periodic refresh (`ir_refresh_mins`, default `15` min):** While
  `g_ir_speed >= 1`, `main.cpp` calls `g_ir.resend(g_ir_speed)` which sets
  `assume_off = true` — so if the purifier missed an earlier wake, it gets the
  Speed-1 wake prefix again before the target speed.
- **Diagnostic `test_ir`:** Sends the requested speed without changing
  `g_core.commanded_speed()`, then re-asserts `g_ir_speed` 10 s later
  (`kTestIrReassertMs` in `main.cpp`).
- **LED feedback:** `on_transmit` callback triggers the P4 white double-flash on
  `GPIO35` (`kDoubleFlashMs = 600 ms`).

## Simulation & Hardware Bring-Up

1. **Host tests (`pio test -e native`):** `ir::nec_raw32`, `ir::nec_encode`, and
   `ir::plan_transition` are pure C++ and covered by `test_TC03_*` in `test_main.cpp`.
2. **Wokwi waveform check:** Wire `GPIO4` to `wokwi-logic-analyzer` and decode with
   `sim.sh ir --expect ...` (checks protocol, `addr 0x00`, command bytes, ~38 kHz
   carrier, and inverse-byte validity).
3. **Hardware bring-up (once board + purifier are available):**
   - Point a phone front camera (usually lacks an IR filter) at the AtomS3 top
     LED during `test_ir` to confirm purple IR flashes.
   - Place AtomS3 within 1–2 m line-of-sight of the IQAir IR window (fallback:
     external M5 Unit IR on Grove `GPIO1`/`GPIO2`).
   - Send each speed (`0..6`) ≥ 2 s apart (safety rule during manual dev loops)
     and confirm: (a) whether Speed `2..6` wakes from standby without Speed 1,
     (b) absolute/idempotent behavior, (c) single vs 2-frame reliability, (d) range.
   - Record results in `.agents/skills/ir-code-capture/references/iqair_gc_multigas.md`.
