#!/usr/bin/env python3
"""
The Heltec's numbered beacon on the laptop's USB and on a power bank: how often each receiver
misses it, per segment. Rules in docs/PHY-notes.md (2026-09-25), declared before the run.

Input is the remote capture: one line per received log line, "time<TAB>receiver<TAB>line". The
beacon numbers itself from 1 at every boot, so a segment - one stretch on one power source - is
where the numbers start again. Within a segment the beacons sent are the span of numbers any
receiver heard.

  bench/analyze_bank.py capture.tsv --order usb,bank,usb [--rx tdeck]
  bench/analyze_bank.py --self-check
"""

import argparse
import json
import math
import re
import sys

RX = re.compile(r"bridge: rx (\d+) bytes: (\S+)")
BEACON = re.compile(r"^(\d{8})A+\.*$")
FAIL = re.compile(r"rx fail hdr")


def load(path):
    rows = []
    for line in open(path):
        parts = line.rstrip("\n").split("\t", 2)
        if len(parts) == 3:
            rows.append((float(parts[0]), parts[1], parts[2]))
    return rows


def segments(rows):
    """[(t_start, t_end, {receiver: set(seq)}, span)] split where the numbering starts again."""
    heard = sorted((t, name, int(m[2][:8])) for t, name, line in rows
                   if (m := RX.search(line)) and BEACON.match(m[2]))
    segs, cur, last = [], None, None
    for t, name, seq in heard:
        if cur is None or (last is not None and seq < last - 5):
            cur = {"t0": t, "t1": t, "seen": {}, "lo": seq, "hi": seq}
            segs.append(cur)
        cur["t1"] = t
        cur["seen"].setdefault(name, set()).add(seq)
        cur["lo"], cur["hi"] = min(cur["lo"], seq), max(cur["hi"], seq)
        last = seq
    return segs


def fisher_less(a, n1, b, n2):
    k, n = a + b, n1 + n2
    return sum(math.comb(n1, i) * math.comb(n2, k - i) for i in range(0, a + 1)
               if 0 <= k - i <= n2) / math.comb(n, k)


def analyse(rows, order, rx):
    segs = segments(rows)
    foreign = [(t, line) for t, name, line in rows
               if name == rx and (m := RX.search(line)) and not BEACON.match(m[2])]
    fails = [t for t, name, line in rows if name == rx and FAIL.search(line)]
    out = {"segments": [], "foreign_at_" + rx: len(foreign)}
    pooled = {}
    for cond, s in zip(order, segs):
        sent = s["hi"] - s["lo"] + 1
        row = {"condition": cond, "from": s["t0"], "to": s["t1"], "beacons": sent,
               "hdr_err": sum(s["t0"] - 3 <= t <= s["t1"] + 3 for t in fails)}
        for name, seqs in sorted(s["seen"].items()):
            row[name] = len(seqs)
        out["segments"].append(row)
        p = pooled.setdefault(cond, {"beacons": 0, rx: 0, "hdr_err": 0})
        p["beacons"] += sent
        p[rx] += row.get(rx, 0)
        p["hdr_err"] += row["hdr_err"]
        for name in s["seen"]:
            if name != rx:
                p[name] = p.get(name, 0) + row[name]
    out["pooled"] = pooled
    if len(segs) != len(order):
        out["verdict"] = f"invalid: {len(segs)} segments found, {len(order)} expected"
        return out
    u, b = pooled["usb"], pooled["bank"]
    mu, mb = u["beacons"] - u[rx], b["beacons"] - b[rx]
    p = fisher_less(mb, b["beacons"], mu, u["beacons"])
    out["miss_usb"], out["miss_bank"], out["p_fewer_on_bank"] = mu, mb, round(p, 5)
    ru, rb = mu / u["beacons"], mb / b["beacons"]
    out["verdict"] = ("invalid: too few beacons" if min(u["beacons"], b["beacons"]) < 100 else
                      "invalid: foreign packets" if foreign else
                      "the host causes the slip" if rb <= 0.03 and ru >= 0.10 and p < 0.01 else
                      "less on the power bank" if ru - rb >= 0.10 and p < 0.05 else
                      "no change detected")
    return out


def self_check():
    rows = [(1.0, "tdeck", "I (1) bridge: rx 38 bytes: 00000010AAAA..."),
            (3.0, "tdeck", "I (3) bridge: rx 38 bytes: 00000011AAAA..."),
            (5.0, "e220", "I (5) bridge: rx 38 bytes: 00000012AAAA..."),
            (7.0, "tdeck", "I (7) sx1262: rx fail hdr rssi -50 dBm, snr 7.00 dB, ..."),
            (9.0, "tdeck", "I (9) bridge: rx 38 bytes: 00000001AAAA..."),
            (11.0, "tdeck", "I (11) bridge: rx 38 bytes: 00000002AAAA...")]
    segs = segments(rows)
    assert len(segs) == 2 and segs[0]["hi"] == 12 and segs[0]["seen"]["tdeck"] == {10, 11}
    assert segs[1]["lo"] == 1 and segs[1]["hi"] == 2
    out = analyse(rows, ["usb", "bank"], "tdeck")
    assert out["segments"][0]["beacons"] == 3 and out["segments"][0]["hdr_err"] == 1
    assert fisher_less(0, 100, 20, 100) < 1e-5
    print("analyze_bank: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", nargs="?")
    ap.add_argument("--order", default="usb,bank,usb")
    ap.add_argument("--rx", default="tdeck")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(load(args.capture), args.order.split(","), args.rx), indent=1))


if __name__ == "__main__":
    sys.exit(main())
