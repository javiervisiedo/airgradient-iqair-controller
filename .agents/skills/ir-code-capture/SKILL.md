---
name: ir-code-capture
description: >-
  Use this skill to look up, import, learn, or verify the air purifier's
  infrared remote codes (IQAir GC MultiGas: speeds 1-6, off, timer): the
  verified code table and its provenance, receiver hardware options for the
  transmit-only AtomS3 Lite, Flipper-IRDB import, and recording labelled
  captures over serial.
---

# Purifier IR Codes

## This project's purifier: already done

The IQAir GC MultiGas codes are **verified against two independent sources**
(user table and Flipper-IRDB) but **not yet tested on hardware**:
[iqair_gc_multigas.md](references/iqair_gc_multigas.md). The raw source file
is [IQAir_GC_Series.ir](references/IQAir_GC_Series.ir).

Use that reference as the single source of truth. Re-capture only if the unit
doesn't respond to those codes during hardware bring-up.

> [!NOTE]
> In `include/sys_config.h`, `data/config.json`, and `SRS §3.2`, 16-bit NEC words
> store the inverse byte in the high byte (`0x<inv><byte>`), so address `0x00`
> is `0xFF00` (`kDefaultIrAddress`) and speeds `0..6` are `{0xFF00, 0xE817,
> 0xFC03, 0xFD02, 0xFA05, 0xFB04, 0xE916}`. Never use byte-swapped `0x00FF` for
> `ir_nec_address` (that transmits address `0xFF`).

## If codes need to be learned or re-checked

> [!IMPORTANT]
> The AtomS3 Lite only **transmits** IR (GPIO4). Learning codes needs a separate
> 38 kHz receiver. Options and wiring: [hardware_options.md](references/hardware_options.md).

**Database import** (no receiver needed):

```bash
python3 .agents/skills/ir-code-capture/scripts/ir_codes_tool.py from-flipper <file.ir> --out <codes.json>
```

It normalizes Flipper's 16-bit NEC command fields to the standard 8-bit command
when the high byte is the inverse.

**Capture with a receiver:** this needs small receive-only firmware that prints
one JSON line per decoded frame. `ir_codes_tool.py capture` expects:

```text
{"ir":{"proto":"NEC","addr":0,"cmd":23,"bits":32,"repeat":false}}
{"ir":{"proto":"UNKNOWN","khz":38,"timings":[9000,4500,560,...]}}
```

Writing that firmware is a project-code task, so agree on it with the user first. Then:

```bash
python3 .agents/skills/ir-code-capture/scripts/ir_codes_tool.py capture \
    --labels speed_1,speed_2,power_off --out <codes.json>
```

The tool asks the user to press each button 3 times, ignores NEC repeat frames,
and flags labels whose captures don't agree.

**Header generation:** `ir_codes_tool.py header <codes.json>` emits one
possible C++ header format. Whether to use it, or that format, is an
architecture decision.

## Recording results

After any capture or hardware test, update the verification log in
[iqair_gc_multigas.md](references/iqair_gc_multigas.md): date, method, result,
range, and repeat count.
