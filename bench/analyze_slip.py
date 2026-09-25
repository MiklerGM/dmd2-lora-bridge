#!/usr/bin/env python3
"""
Does a transmit setting at the sync/SFD end of the preamble change how often our receiver slips a
symbol? Rules in docs/PHY-notes.md (2026-09-25), declared before the run.

Per Heltec arm: delivery at every receiver, and at the T-Deck its misses split into header errors
(the slip) and silence. Each arm against the control arm by a one-sided Fisher exact test on the
T-Deck's misses.

  bench/analyze_slip.py run.json [--rx tdeck] [--control s54] [--ref e22]
  bench/analyze_slip.py --self-check
"""

import argparse
import json
import math
import sys

from analyze_fail import parse


def fisher_less(a, n1, b, n2):
    """P(misses in arm <= a) given the margins, arm n1 against control n2 with b misses."""
    k, n = a + b, n1 + n2
    return sum(math.comb(n1, i) * math.comb(n2, k - i) for i in range(0, a + 1)
               if 0 <= k - i <= n2) / math.comb(n, k)


def analyse(run, rx, control, ref):
    sent = {int(k): v for k, v in run["sent"].items()}
    starts = {int(k): v[0] for k, v in run["tx_times"].items()}
    _, fails = parse(run["log"][rx], run["log_t"][rx], starts)
    hdr = {f["seq"] for f in fails if f["seq"] is not None and f["kind"] == "hdr"}
    seen = {r: {int(x) for x in v} for r, v in run["seen"].items()}
    arms = sorted(set(sent.values()))
    out = {"arms": {}}
    for a in arms:
        seqs = [s for s in sent if sent[s] == a]
        miss = [s for s in seqs if s not in seen[rx]]
        out["arms"][a] = {"n": len(seqs), rx: len(seqs) - len(miss), "hdr_err": sum(s in hdr for s in miss),
                          "silent": sum(s not in hdr for s in miss),
                          **{r: sum(s in seen[r] for s in seqs) for r in seen if r != rx}}
    c = out["arms"][control]
    c_miss = c["n"] - c[rx]
    ref_ok = out["arms"][ref][rx] >= 0.9 * out["arms"][ref]["n"]
    for a, v in out["arms"].items():
        if a in (control, ref):
            continue
        miss = v["n"] - v[rx]
        p = fisher_less(miss, v["n"], c_miss, c["n"])
        v["p_fewer_misses"] = round(p, 5)
        v["verdict"] = ("invalid" if not ref_ok else
                        "not received" if v[rx] < 0.5 * v["n"] and v["hdr_err"] < 0.5 * miss else
                        "removes the slip" if miss <= 0.03 * v["n"] and c_miss >= 0.1 * c["n"] and p < 0.01 else
                        "fewer misses" if (c_miss / c["n"] - miss / v["n"]) >= 0.10 and p < 0.05 else
                        "no change detected")
    out["valid"] = ref_ok
    return out


def self_check():
    assert abs(fisher_less(0, 70, 12, 70) - math.comb(70, 12) / math.comb(140, 12)) < 1e-12
    assert fisher_less(12, 70, 12, 70) > 0.5
    assert fisher_less(0, 70, 12, 70) < 0.001
    print("analyze_slip: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--rx", default="tdeck")
    ap.add_argument("--control", default="s54")
    ap.add_argument("--ref", default="e22")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.rx, args.control, args.ref), indent=1))


if __name__ == "__main__":
    sys.exit(main())
