# IQAir GC MultiGas: IR Codes (verified reference)

**Status:** cross-verified against two independent sources. **Not yet tested on hardware.**

## Codes (NEC, 38 kHz, address 0x00)

| Function | NEC addr | NEC cmd | 32-bit frame (IRremote `decodedRawData`) | 16-bit form (addr / cmd with inverse) |
| :--- | :--- | :--- | :--- | :--- |
| Speed 1 | 0x00 | 0x17 | `0xE817FF00` | 0xFF00 / 0xE817 |
| Speed 2 | 0x00 | 0x03 | `0xFC03FF00` | 0xFF00 / 0xFC03 |
| Speed 3 | 0x00 | 0x02 | `0xFD02FF00` | 0xFF00 / 0xFD02 |
| Speed 4 | 0x00 | 0x05 | `0xFA05FF00` | 0xFF00 / 0xFA05 |
| Speed 5 | 0x00 | 0x04 | `0xFB04FF00` | 0xFF00 / 0xFB04 |
| Speed 6 | 0x00 | 0x16 | `0xE916FF00` | 0xFF00 / 0xE916 |
| Off (standby) | 0x00 | 0x00 | `0xFF00FF00` | 0xFF00 / 0xFF00 |
| Timer on | 0x00 | 0x15 | `0xEA15FF00` | (Flipper file only) |
| Timer (cycle) | 0x00 | 0x14 | `0xEB14FF00` | (Flipper file only) |
| Timer off | 0x00 | 0x01 | `0xFE01FF00` | (Flipper file only) |

The "16-bit" values are standard 8-bit NEC values with the bitwise inverse
attached as the high byte. Send them as standard NEC with address 0x00 and the
8-bit command. Don't send them as "NEC extended" with 16-bit fields.

## Provenance / verification log

1. **User-supplied table (2026-11-06):** speeds 1–6 and Off in the 16-bit form above.
2. **Structural check:** every entry is a valid standard NEC frame (inverse bytes match).
3. **Independent source:** [IQAir_GC_Series.ir](IQAir_GC_Series.ir) from Flipper-IRDB
   (`Air_Purifiers/IQAir/`, added 2024-03-24 by Andrew R. DeFilippis, header
   "IQAir GC Series (GC MultiGas)"). All 7 user codes match exactly. The file also
   contains the 3 timer codes.
4. **Hardware: TODO.** When it's done, record the date, the result, the range, and the repeat count here.

## Behaviour assumptions to confirm on the unit

| Assumption | Why it matters |
| :--- | :--- |
| There's no power-on code; a speed code wakes the unit from standby | Decides how "turn on at speed N" works |
| Speed codes are absolute (idempotent) | If so, resending is safe and fixes drift caused by the physical remote |
| A single frame (no repeats) is accepted | Send-repeat setting |
| Usable range and angle from the AtomS3's built-in LED | Mounting position, or an external IR emitter |
| Whether the unit beeps or shows feedback per command | Manual verification during bring-up |

## Risks

- **Address 0x00 is the most common NEC address.** Generic LED strips and other
  cheap remotes use it, some with commands in the same 0x00–0x17 range. Sends may
  affect other devices, and other remotes may change the purifier.
