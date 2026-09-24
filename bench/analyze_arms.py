#!/usr/bin/env python3
"""
Arms against a control arm, judged by EBYTE delivery pooled over several receivers.

Rules declared in docs/PHY-notes.md (the 2026-09-23 series, tests A and B): D = deliveries at
the target receivers / (targets x n); an arm whose D beats the control's by at least --points is
an effect; one at least --points below is reported as below, not as an effect. Valid only if the
reference reaches every target at 90 % or more.

  bench/analyze_arms.py run.json --control s54 [--targets e22,e220b] [--ref ref] [--points 20]
"""

import argparse
import json
import re
import sys

TXCFG = re.compile(r"txcfg seq=(\d+) ldo=(-?\d) reg_cmd=(\S+) txmod=(-?\d) 0889=([0-9A-F]{2})")


def analyse(run, control, targets, ref, points):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {k: {int(x) for x in v} for k, v in run["seen"].items()}
    arms = sorted(set(sent.values()))
    out = {"arms": {}}
    for arm in arms:
        seqs = [s for s, w in sent.items() if w == arm]
        row = {rx: sum(s in seen[rx] for s in seqs) for rx in seen}
        row["n"] = len(seqs)
        row["D"] = round(sum(row[t] for t in targets) / (len(targets) * len(seqs)), 3) if seqs else None
        out["arms"][arm] = row
    ref_row = out["arms"][ref]
    out["valid"] = all(ref_row[t] >= 0.9 * ref_row["n"] for t in targets)
    base = out["arms"][control]["D"]
    for arm, row in out["arms"].items():
        if arm in (ref, control):
            continue
        delta = round(100 * (row["D"] - base), 1)
        row["delta_points"] = delta
        row["verdict"] = "effect" if delta >= points else "below" if delta <= -points else "none"
    return out


def txcfg_check(run, arms_tokens):
    """Every token packet must have read back what it asked for. arms_tokens: arm -> (ldo, txmod)."""
    sent = {int(k): v for k, v in run["sent"].items()}
    logs = [l for lines in run.get("tx_log", {}).values() for l in lines]
    seen = {}
    for line in logs:
        m = TXCFG.search(line)
        if m:
            seen[int(m[1])] = (int(m[2]), m[3], int(m[4]), int(m[5], 16))
    bad = []
    for s, arm in sent.items():
        if arm not in arms_tokens:
            continue
        want_ldo, want_txmod = arms_tokens[arm]
        got = seen.get(s)
        if (got is None or got[0] != want_ldo or (want_ldo >= 0 and got[1] != "ok")
                or got[2] != want_txmod or (want_txmod >= 0 and bool(got[3] & 0x04) != bool(want_txmod))):
            bad.append((s, arm, got))
    return bad


TXCAL = re.compile(r"txcal seq=(\d+) cal=(\d) rejected=(\d) errors=([0-9A-F]{4})")
CAL_ERRORS = 0x000F   # RC64k, RC13M, PLL, ADC calibration failures


def txcal_check(run, arms_cal):
    """Every cal packet logged; a cal=1 packet must not be rejected or report a calibration error."""
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {}
    for line in (l for lines in run.get("tx_log", {}).values() for l in lines):
        m = TXCAL.search(line)
        if m:
            seen[int(m[1])] = (int(m[2]), int(m[3]), int(m[4], 16))
    bad = []
    for s, arm in sent.items():
        if arm not in arms_cal:
            continue
        got = seen.get(s)
        if got is None or got[0] != arms_cal[arm] or (got[0] and (got[1] or got[2] & CAL_ERRORS)):
            bad.append((s, arm, got))
    return bad


def self_check():
    run = {"sent": {"1": "c", "2": "x", "3": "ref", "4": "c", "5": "x", "6": "ref"},
           "seen": {"e22": [1, 2, 5, 3, 6], "e220b": [2, 5, 3, 6]}}
    out = analyse(run, "c", ["e22", "e220b"], "ref", 20)
    assert out["valid"] and out["arms"]["c"]["D"] == 0.25 and out["arms"]["x"]["D"] == 1.0
    assert out["arms"]["x"]["verdict"] == "effect" and out["arms"]["x"]["delta_points"] == 75.0
    run["tx_log"] = {"tdeck": ["txcfg seq=2 ldo=1 reg_cmd=ok txmod=0 0889=0A",
                               "txcfg seq=5 ldo=1 reg_cmd=ok txmod=0 0889=0E"]}
    assert [b[0] for b in txcfg_check(run, {"x": (1, 0)})] == [5]
    run["tx_log"] = {"tdeck": ["txcal seq=2 cal=1 rejected=0 errors=0020",
                               "txcal seq=5 cal=1 rejected=0 errors=0004"]}
    assert [b[0] for b in txcal_check(run, {"x": 1})] == [5]   # PLL calibration error flagged
    print("analyze_arms: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--control")
    ap.add_argument("--targets", default="e22,e220b")
    ap.add_argument("--ref", default="ref")
    ap.add_argument("--points", type=float, default=20)
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.control, args.targets.split(","),
                             args.ref, args.points), indent=1))


if __name__ == "__main__":
    sys.exit(main())
