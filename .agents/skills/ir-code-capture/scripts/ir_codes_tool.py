#!/usr/bin/env python3
"""Purifier IR code tool.

Subcommands:
  capture --labels power,fan_up,... [--port P] [--secs 12] [--presses 3] --out codes.json
      Listen to the ir_dump firmware over serial. For each label, ask the user to
      press that remote button `presses` times within `secs` seconds. Merges into
      --out if it already exists.
  from-flipper FILE.ir --out codes.json
      Import a Flipper-IRDB .ir file (parsed + raw signals).
  header codes.json
      Print one possible C++ header (ir_codes.h) to stdout.

codes.json schema:
  {"device": "...", "codes": {"<label>": {"proto": "NEC", "addr": 4, "cmd": 8, "bits": 32}
                               | {"proto": "RAW", "khz": 38, "timings": [9000, 4500, ...]}}}
"""
import argparse
import glob
import json
import os
import pathlib
import re
import shutil
import sys
import time
from collections import Counter

SUPPORTED = {"NEC", "NEC2", "Onkyo", "Apple", "Samsung", "Samsung48", "SamsungLG", "Sony",
             "RC5", "RC6", "Panasonic", "LG", "LG2", "JVC", "Denon", "Sharp"}
# IRremote protocol string -> canonical name used in codes.json / header
CANON = {"NEC": "NEC", "NEC2": "NEC", "Onkyo": "NEC", "Apple": "NEC",
         "Samsung": "SAMSUNG", "Samsung48": "SAMSUNG", "SamsungLG": "SAMSUNG",
         "Sony": "SONY", "RC5": "RC5", "RC6": "RC6", "Panasonic": "PANASONIC",
         "LG": "LG", "LG2": "LG", "JVC": "JVC", "Denon": "DENON", "Sharp": "DENON"}
FLIPPER = {"NEC": ("NEC", 32), "NECext": ("NEC", 32),
           "Samsung32": ("SAMSUNG", 32), "RC5": ("RC5", 13), "RC5X": ("RC5", 13),
           "RC6": ("RC6", 20), "SIRC": ("SONY", 12), "SIRC15": ("SONY", 15),
           "SIRC20": ("SONY", 20)}


# ---------------------------------------------------------------- utilities
def ensure_pyserial():
    try:
        import serial  # noqa: F401
        return
    except ImportError:
        pass
    pio = shutil.which("pio")
    if not pio:
        penv_pio = pathlib.Path.home() / ".platformio" / "penv" / "bin" / "pio"
        if penv_pio.exists():
            pio = str(penv_pio)
    if pio and not os.environ.get("_IRTOOL_REEXEC"):
        py = open(pio).readline().strip().lstrip("#!").strip()
        if os.path.exists(py):
            os.environ["_IRTOOL_REEXEC"] = "1"
            os.execv(py, [py] + sys.argv)
    sys.exit("ERROR: pyserial missing. `python3 -m pip install pyserial`")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("ERROR: no AtomS3 serial port found")
    return ports[0]


def load(path):
    p = pathlib.Path(path)
    return json.loads(p.read_text()) if p.exists() else {"device": "", "codes": {}}


def save(path, data):
    pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(path).write_text(json.dumps(data, indent=2) + "\n")
    print(f"saved {path}", file=sys.stderr)


def ident(rec):
    if rec["proto"] == "RAW":
        return ("RAW", len(rec["timings"]) // 4)  # coarse: same length bucket
    return (rec["proto"], rec["addr"], rec["cmd"])


# ---------------------------------------------------------------- capture
def capture(args):
    ensure_pyserial()
    import serial
    port = args.port or find_port()
    data = load(args.out)
    s = serial.Serial(port, 115200, timeout=0.2)
    print(f"listening on {port}", file=sys.stderr)
    bad = []
    for label in [x.strip() for x in args.labels.split(",") if x.strip()]:
        print(f"\n>>> Press [{label}] {args.presses}x now (aim at receiver, 5-30 cm). "
              f"Listening {args.secs}s...", flush=True)
        s.reset_input_buffer()
        frames, end = [], time.time() + args.secs
        while time.time() < end and len(frames) < args.presses:
            line = s.readline().decode("utf-8", "replace").strip()
            if not line.startswith("{"):
                continue
            try:
                ir = json.loads(line).get("ir")
            except json.JSONDecodeError:
                continue
            if not ir or ir.get("repeat"):
                continue
            if ir["proto"] in SUPPORTED:
                rec = {"proto": CANON[ir["proto"]], "addr": ir["addr"], "cmd": ir["cmd"],
                       "bits": ir["bits"]}
            elif "timings" in ir and len(ir["timings"]) >= 8:
                rec = {"proto": "RAW", "khz": ir.get("khz", 38), "timings": ir["timings"]}
            else:
                continue
            frames.append(rec)
            print(f"    got {ident(rec)}", flush=True)
        if not frames:
            print(f"!!! [{label}] nothing received", flush=True)
            bad.append(label)
            continue
        counts = Counter(ident(f) for f in frames)
        best, n = counts.most_common(1)[0]
        chosen = next(f for f in frames if ident(f) == best)
        if n < len(frames) or n < min(2, args.presses):
            print(f"!!! [{label}] inconsistent ({dict(counts)}), re-capture recommended", flush=True)
            bad.append(label)
        data["codes"][label] = chosen
    save(args.out, data)
    if bad:
        print(f"\nRE-CAPTURE: {', '.join(bad)}", file=sys.stderr)
        sys.exit(1)


# ---------------------------------------------------------------- flipper
def le_hex(s):
    return int.from_bytes(bytes(int(b, 16) for b in s.split()), "little")


def from_flipper(args):
    data = load(args.out)
    blocks = re.split(r"^#.*$", pathlib.Path(args.file).read_text(), flags=re.M)
    for b in blocks:
        kv = dict(re.findall(r"^(\w+):\s*(.*)$", b, flags=re.M))
        if "name" not in kv:
            continue
        label = re.sub(r"\W+", "_", kv["name"].strip().lower()).strip("_")
        if kv.get("type") == "parsed":
            if kv["protocol"] not in FLIPPER:
                print(f"skip {label}: unsupported protocol {kv['protocol']}", file=sys.stderr)
                continue
            proto, bits = FLIPPER[kv["protocol"]]
            addr, cmd = le_hex(kv["address"]), le_hex(kv["command"])
            if proto == "NEC" and cmd > 0xFF and (cmd >> 8) == (~cmd & 0xFF):
                cmd &= 0xFF  # high byte is just the inverted command (standard NEC)
            # A cmd > 0xFF left here means a true 16-bit NEC command; send it
            # as a raw 32-bit frame: (cmd << 16) | addr.
            data["codes"][label] = {"proto": proto, "addr": addr, "cmd": cmd, "bits": bits}
        elif kv.get("type") == "raw":
            data["codes"][label] = {"proto": "RAW",
                                    "khz": round(int(kv.get("frequency", 38000)) / 1000),
                                    "timings": [abs(int(x)) for x in kv["data"].split()]}
        print(f"imported {label}: {data['codes'].get(label, {}).get('proto')}", file=sys.stderr)
    data["device"] = data.get("device") or pathlib.Path(args.file).stem
    save(args.out, data)


# ---------------------------------------------------------------- header
def header(args):
    data = load(args.codes)
    out = ["// Generated by .agents/skills/ir-code-capture/scripts/ir_codes_tool.py",
           f"// Device: {data.get('device', '')}", "#pragma once", "#include <stdint.h>", "",
           "enum class IrProto : uint8_t { NEC, SAMSUNG, SONY, RC5, RC6, PANASONIC, LG, JVC, "
           "DENON, RAW };", "",
           "struct IrCode {", "  const char* name;", "  IrProto proto;", "  uint16_t address;",
           "  uint16_t command;", "  uint8_t bits;", "  const uint16_t* raw;  // RAW only",
           "  uint16_t rawLen;", "  uint8_t khz;", "};", "", "namespace ir {"]
    names = []
    for label, c in sorted(data["codes"].items()):
        ident_ = "k" + "".join(p.capitalize() for p in label.split("_"))
        names.append(ident_)
        if c["proto"] == "RAW":
            t = ", ".join(str(int(x)) for x in c["timings"])
            out.append(f"static const uint16_t {ident_}Raw[] = {{{t}}};")
            out.append(f'constexpr IrCode {ident_}{{"{label}", IrProto::RAW, 0, 0, 0, '
                       f"{ident_}Raw, {len(c['timings'])}, {c.get('khz', 38)}}};")
        else:
            out.append(f'constexpr IrCode {ident_}{{"{label}", IrProto::{c["proto"]}, '
                       f"0x{c['addr']:X}, 0x{c['cmd']:X}, {c.get('bits', 0)}, nullptr, 0, 38}};")
    out += ["", f"constexpr const IrCode* kAll[] = {{{', '.join('&' + n for n in names)}}};",
            "}  // namespace ir", ""]
    print("\n".join(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("--labels", required=True)
    c.add_argument("--port")
    c.add_argument("--secs", type=int, default=12)
    c.add_argument("--presses", type=int, default=3)
    c.add_argument("--out", required=True)
    f = sub.add_parser("from-flipper")
    f.add_argument("file")
    f.add_argument("--out", required=True)
    h = sub.add_parser("header")
    h.add_argument("codes")
    a = ap.parse_args()
    {"capture": capture, "from-flipper": from_flipper, "header": header}[a.cmd](a)


if __name__ == "__main__":
    main()
