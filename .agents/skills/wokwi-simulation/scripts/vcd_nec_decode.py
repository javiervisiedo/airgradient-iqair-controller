#!/usr/bin/env python3
"""Decode NEC IR frames from a Wokwi logic-analyzer VCD capture (stdlib only).

Wire the IR TX GPIO to a logic-analyzer channel in diagram.json, run the
simulation with `wokwi-cli --vcd-file sim-out/ir.vcd`, then:

  vcd_nec_decode.py sim-out/ir.vcd                       # list decoded frames
  vcd_nec_decode.py sim-out/ir.vcd --channel IR          # pick channel by name (default: first)
  vcd_nec_decode.py sim-out/ir.vcd --expect speed_1,power_off
  vcd_nec_decode.py sim-out/ir.vcd --expect 0x00:0x17    # addr:cmd pairs also accepted
  vcd_nec_decode.py --selftest

Output: one JSON object per frame. Exit code 1 if --expect doesn't match,
or if no frames were found. Handles a modulated (38 kHz carrier) or an
unmodulated (envelope) signal, active-high by default (--invert for active-low).
"""
import argparse
import json
import re
import sys

# IQAir GC MultiGas (see .agents/skills/ir-code-capture/references/iqair_gc_multigas.md)
GC_CODES = {0x17: "speed_1", 0x03: "speed_2", 0x02: "speed_3", 0x05: "speed_4",
            0x04: "speed_5", 0x16: "speed_6", 0x00: "power_off",
            0x15: "timer_on", 0x14: "timer", 0x01: "timer_off"}
GC_ADDR = 0x00
UNITS = {"s": 1e6, "ms": 1e3, "us": 1.0, "ns": 1e-3, "ps": 1e-6, "fs": 1e-9}


def parse_vcd(text, channel=None):
    m = re.search(r"\$timescale\s*(\d+)\s*(\w+)\s*\$end", text)
    scale_us = int(m.group(1)) * UNITS[m.group(2)] if m else 1e-3  # default 1ns
    ids = {}
    for vm in re.finditer(r"\$var\s+\w+\s+\d+\s+(\S+)\s+(\S+)(?:\s+\[[^\]]*\])?\s+\$end", text):
        ids[vm.group(2)] = vm.group(1)
    if not ids:
        sys.exit("ERROR: no $var signals in VCD")
    if channel is None:
        name = next(iter(ids))
    elif channel in ids:
        name = channel
    else:
        sys.exit(f"ERROR: channel {channel!r} not in VCD (have: {', '.join(ids)})")
    sid = ids[name]
    body = text[text.find("$enddefinitions"):]
    t, trans, last = 0.0, [], None
    for tok in body.split():
        if tok.startswith("#"):
            t = int(tok[1:]) * scale_us
        elif len(tok) >= 2 and tok[0] in "01xXzZ" and tok[1:] == sid:
            v = 1 if tok[0] == "1" else 0
            if v != last:
                trans.append((t, v))
                last = v
    return name, trans


def envelope(trans, invert=False, carrier_gap_us=100.0):
    """Merge carrier pulses into marks. Returns [(start_us, end_us, rising_edges)]."""
    marks, cur = [], None
    for i, (t, v) in enumerate(trans):
        v = 1 - v if invert else v
        if v == 1:
            if cur and t - cur[1] <= carrier_gap_us:
                cur[2] += 1
            else:
                if cur:
                    marks.append(tuple(cur))
                cur = [t, t, 1]
        elif cur:
            cur[1] = t
    if cur:
        marks.append(tuple(cur))
    return [m for m in marks if m[1] > m[0]]


def near(x, target, tol=0.3):
    return abs(x - target) <= target * tol


def decode(marks, frame_gap_us=20000.0):
    frames, i = [], 0
    while i < len(marks):
        s, e, edges = marks[i]
        mark = e - s
        nxt = marks[i + 1][0] if i + 1 < len(marks) else None
        space = (nxt - e) if nxt is not None else None
        khz = round(edges / mark * 1000, 1) if edges > 2 else None
        if near(mark, 9000) and space is not None and near(space, 2250, 0.35):
            frames.append({"t_ms": round(s / 1000, 2), "proto": "NEC", "repeat": True})
            i += 2
            continue
        if near(mark, 9000) and space is not None and near(space, 4500):
            bits, j = [], i + 1
            while len(bits) < 32 and j + 1 < len(marks):
                bm = marks[j][1] - marks[j][0]
                bs = marks[j + 1][0] - marks[j][1]
                if not near(bm, 560, 0.5):
                    break
                if near(bs, 560, 0.5):
                    bits.append(0)
                elif near(bs, 1690, 0.35):
                    bits.append(1)
                else:
                    break
                j += 1
            if len(bits) == 32:
                raw = sum(b << k for k, b in enumerate(bits))  # LSB first
                a, ai, c, ci = raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF, raw >> 24
                f = {"t_ms": round(s / 1000, 2), "proto": "NEC", "addr": a, "cmd": c,
                     "raw": f"0x{raw:08X}", "valid": ai == (~a & 0xFF) and ci == (~c & 0xFF),
                     "carrier_khz": khz}
                if a == GC_ADDR and c in GC_CODES:
                    f["label"] = GC_CODES[c]
                frames.append(f)
                i = j + 1
                continue
            frames.append({"t_ms": round(s / 1000, 2), "proto": "NEC?", "error": f"only {len(bits)} bits"})
            i = j + 1
            continue
        i += 1
    return frames


def check_expect(frames, expect):
    want = []
    for item in expect.split(","):
        item = item.strip()
        if ":" in item:
            a, c = item.split(":")
            want.append((int(a, 0), int(c, 0)))
        else:
            inv = {v: k for k, v in GC_CODES.items()}
            if item not in inv:
                sys.exit(f"ERROR: unknown label {item!r}")
            want.append((GC_ADDR, inv[item]))
    got = [(f["addr"], f["cmd"]) for f in frames if "cmd" in f and f["valid"]]
    return got == want, got, want


def selftest():
    def synth(cmd, addr=0x00, t0=0.0, khz=38.0):
        period = 1e6 / (khz * 1000)
        out, t = [], t0
        def mark(d):
            nonlocal t
            end = t + d
            while t < end:
                out.append((t, 1)); out.append((t + period / 3, 0)); t += period
            t = end
        def space(d):
            nonlocal t
            t += d
        raw = addr | ((~addr & 0xFF) << 8) | (cmd << 16) | ((~cmd & 0xFF) << 24)
        mark(9000); space(4500)
        for k in range(32):
            mark(560); space(1690 if (raw >> k) & 1 else 560)
        mark(560)
        return out, t
    tr1, t = synth(0x17)
    tr2, _ = synth(0x00, t0=t + 200000)
    lines = ["$timescale 1ns $end", "$scope module logic $end", "$var wire 1 ! IR $end",
             "$upscope $end", "$enddefinitions $end", "#0", "0!"]
    for tt, v in tr1 + tr2:
        lines += [f"#{int(tt * 1000)}", f"{v}!"]
    name, trans = parse_vcd("\n".join(lines))
    frames = decode(envelope(trans))
    ok, got, want = check_expect(frames, "speed_1,power_off")
    print(json.dumps(frames, indent=1))
    print("SELFTEST", "PASS" if ok and all(f.get("carrier_khz") for f in frames) else "FAIL")
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("vcd", nargs="?")
    ap.add_argument("--channel", help="VCD signal name (logic analyzer channelNames)")
    ap.add_argument("--invert", action="store_true", help="signal is active-low")
    ap.add_argument("--expect", help="comma list of GC labels or addr:cmd pairs, in order")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
    if not a.vcd:
        ap.error("vcd file required")
    name, trans = parse_vcd(open(a.vcd).read(), a.channel)
    frames = decode(envelope(trans, a.invert))
    print(f"# channel={name} transitions={len(trans)} frames={len(frames)}", file=sys.stderr)
    for f in frames:
        print(json.dumps(f))
    if not frames:
        sys.exit(1)
    if a.expect:
        ok, got, want = check_expect(frames, a.expect)
        fmt = lambda xs: ", ".join(f"0x{x:02X}:0x{y:02X}" for x, y in xs)
        print(f"# expect {'PASS' if ok else 'FAIL'}: got [{fmt(got)}] want [{fmt(want)}]", file=sys.stderr)
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
