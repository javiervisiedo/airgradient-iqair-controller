# IR Receiver Hardware Options for the AtomS3 Lite

The AtomS3 Lite's IR LED on GPIO4 only transmits. Pick one option below.

## Option 1: M5Stack Unit IR (SKU U002), recommended

- It plugs into **Grove Port A** (G1/G2) with no soldering. It has both an IR
  receiver and a transmitter LED.
- The receiver output lands on one Grove signal line. Start the dump firmware with
  `-DIR_RX_PIN=1`. If nothing prints when you press remote buttons, rebuild with
  `-DIR_RX_PIN=2`.
- Bonus: the unit's own IR LED is stronger than the built-in one. If range is a
  problem later, you can transmit through it (on the other Grove pin).

## Option 2: Bare 38 kHz receiver module (VS1838B, TSOP38238, TSOP4838)

| Receiver pin | AtomS3 Lite |
| :--- | :--- |
| VCC (VS) | **3V3** (not 5 V: the output would exceed the ESP32-S3's 3.3 V max) |
| GND | GND |
| OUT | G1 (Grove) or G5/G6/G7/G8 (bottom header) |

Check the pinout for your exact part. TSOP382xx is OUT-GND-VS, VS1838B modules
vary. The output is active-low, which IRremote expects.

## Option 3: No receiver

- **Flipper-IRDB** (`Lucaslhm/Flipper-IRDB`), folders `Air_Purifiers/` and `Fans/`.
- **IRDB** (`probonopd/irdb`) CSV files: protocol, device, subdevice, function.
- A universal learning remote or a Broadlink exported as Pronto hex can also be
  converted (store the Pronto string as a raw code).

Without a receiver, you can only verify codes by sending them and watching the
purifier.

## Capture tips

- Hold the remote 5–30 cm from the receiver. Avoid direct sunlight and LED bulbs
  (they cause noise and spurious `UNKNOWN` frames).
- Press briefly. Holding a button produces NEC repeat frames
  (`flags=REPEAT`), which the tool ignores.
- Some purifiers send long frames (more than 100 marks/spaces). The dump firmware sets
  `RAW_BUFFER_LENGTH 750` to fit them.
