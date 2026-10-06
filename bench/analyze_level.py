#!/usr/bin/env python3
"""
A weak and a strong receiver of the same part on the same packets, and the SNR an SX1262 reports
for two transmitters at equal RSSI. Rules in docs/PHY-notes.md (2026-09-24), declared before
the run.

  bench/analyze_level.py run.json [--arm heltec] [--ref e22] [--weak e220b] [--strong e220]
                         [--snr-rx tdeck]
"""

import argparse
import collections
import json
import math
import re
import sys

QUALITY = re.compile(r"rx quality rssi (-?\d+) dBm, snr (-?\d+)\.(\d+) dB")
RX = re.compile(r"bridge: rx \d+ bytes(?:, rssi -?\d+)?: (\d{8})")


def mcnemar_exact(b, c):
    """Two-sided exact McNemar p from the discordant counts."""
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    tail = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def quality_by_seq(lines):
    """seq -> (rssi dBm, snr dB) for packets the receiver delivered."""
    out, cur = {}, None
    for line in lines:
        m = QUALITY.search(line)
        if m:
            whole, frac = int(m[2]), int(m[3])
            snr = abs(whole) + frac / 100.0
            cur = (int(m[1]), -snr if m[2].startswith("-") else snr)
        m = RX.search(line)
        if m:
            if cur is not None:
                out[int(m[1])] = cur
            cur = None
    return out


def snr_at_equal_rssi(q, sent, arm, ref):
    """Mean SNR difference ref - arm within 1 dB RSSI bins, weighted by the smaller count."""
    bins = collections.defaultdict(lambda: {arm: [], ref: []})
    for s, (rssi, snr) in q.items():
        if sent.get(s) in (arm, ref):
            bins[rssi][sent[s]].append(snr)
    num = den = var = 0.0
    used = {}
    for rssi, g in sorted(bins.items()):
        a, r = g[arm], g[ref]
        if len(a) < 2 or len(r) < 2:
            continue
        w = min(len(a), len(r))
        d = sum(r) / len(r) - sum(a) / len(a)
        va = sum((x - sum(a) / len(a)) ** 2 for x in a) / (len(a) - 1)
        vr = sum((x - sum(r) / len(r)) ** 2 for x in r) / (len(r) - 1)
        num += w * d
        den += w
        var += w * w * (va / len(a) + vr / len(r))
        used[rssi] = (len(a), len(r), round(d, 2))
    if not den:
        return None
    diff, se = num / den, math.sqrt(var) / den
    return {"ref_minus_arm_db": round(diff, 2), "ci95": [round(diff - 1.96 * se, 2), round(diff + 1.96 * se, 2)],
            "bins_rssi_(n_arm,n_ref,diff)": used}


def analyse(run, arm, ref, weak, strong, snr_rx):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {k: {int(x) for x in v} for k, v in run["seen"].items()}
    seqs = [s for s, w in sent.items() if w == arm]
    refs = [s for s, w in sent.items() if w == ref]
    out = {"ref": {rx: f"{sum(s in seen[rx] for s in refs)}/{len(refs)}" for rx in seen},
           "arm": {rx: f"{sum(s in seen[rx] for s in seqs)}/{len(seqs)}" for rx in seen}}
    b = sum(s in seen[weak] and s not in seen[strong] for s in seqs)   # weak only
    c = sum(s in seen[strong] and s not in seen[weak] for s in seqs)   # strong only
    diff = 100 * (b - c) / len(seqs)
    out["weak_minus_strong_points"] = round(diff, 1)
    out["discordant_weak_only_strong_only"] = [b, c]
    out["mcnemar_p"] = round(mcnemar_exact(b, c), 5)
    valid = all(sum(s in seen[rx] for s in refs) >= 0.9 * len(refs) for rx in (weak, strong))
    out["valid"] = valid
    out["verdict"] = ("invalid" if not valid else
                      "level matters" if abs(diff) >= 20 and out["mcnemar_p"] < 0.01 else
                      "no effect detected at this sensitivity")
    q = quality_by_seq(run["log"][snr_rx])
    out["snr"] = snr_at_equal_rssi(q, sent, arm, ref)
    if out["snr"]:
        lo, hi = out["snr"]["ci95"]
        out["snr"]["verdict"] = ("ours lower" if out["snr"]["ref_minus_arm_db"] >= 1 and lo > 0 else
                                 "ours not lower" if hi < 1 else "inconclusive")
    return out


def self_check():
    assert mcnemar_exact(0, 0) == 1.0 and mcnemar_exact(20, 0) < 1e-5
    assert abs(mcnemar_exact(5, 5) - 1.0) < 1e-12
    lines = ["rx quality rssi -103 dBm, snr 0.75 dB, signal rssi -110 dBm",
             "I (1) bridge: rx 38 bytes: 00000001AAAA...",
             "rx quality rssi -104 dBm, snr -1.25 dB, signal rssi -111 dBm",
             "I (2) bridge: rx 38 bytes: 00000002AAAA..."]
    assert quality_by_seq(lines) == {1: (-103, 0.75), 2: (-104, -1.25)}
    q = {1: (-103, 1.0), 2: (-103, 2.0), 3: (-103, 0.0), 4: (-103, 1.0)}
    sent = {1: "a", 2: "a", 3: "r", 4: "r"}
    assert snr_at_equal_rssi(q, sent, "a", "r")["ref_minus_arm_db"] == -1.0
    print("analyze_level: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--arm", default="heltec")
    ap.add_argument("--ref", default="e22")
    ap.add_argument("--weak", default="e220b")
    ap.add_argument("--strong", default="e220")
    ap.add_argument("--snr-rx", default="tdeck")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.arm, args.ref, args.weak, args.strong,
                             args.snr_rx), indent=1))


if __name__ == "__main__":
    sys.exit(main())
