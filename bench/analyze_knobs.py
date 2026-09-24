#!/usr/bin/env python3
"""
Screen transmit settings by the shape of the signal right after a symbol's jump, not by delivery.
Rules in docs/PHY-notes.md (2026-09-24), declared before the capture.

At five slices 64-187 us after the opening jumps of header symbols h6 and h7 (the landmarks at 17.98
and 18.58), each test arm is compared with the control arm, and the E22 with the control as the
positive control: the run can only speak if the E22's known lead shows in it.

  bench/analyze_knobs.py capture.json.gz --control ctl --arms txm ldo cal [--ref e22]
"""

import argparse
import json
import sys

from analyze_scope_timing import analyse

SLICES = {-107: 17.98, -100: 17.98, 93: 18.58, 100: 18.58, 108: 18.58}


def at_slices(result):
    out = {}
    for o in result:
        pos = SLICES.get(o["offset_khz"])
        if pos is None:
            continue
        near = [r for r in o["landmarks"] if abs(r["symbol"] - pos) <= 0.12]
        r = min(near, key=lambda r: abs(r["symbol"] - pos)) if near else None
        out[o["offset_khz"]] = {"diff_us": r["b_minus_a_us"], "ci_us": r["ci_us"], "n": r["n"]} if r else None
    return out


def count(points, sign, size=30.0):
    """Slices where the difference is at least size in that direction with its interval clear of 0."""
    return sum(1 for p in points.values() if p and sign * p["diff_us"] >= size
               and (p["ci_us"][1] < 0 if sign < 0 else p["ci_us"][0] > 0))


def judge(points):
    earlier, later = count(points, -1), count(points, +1)
    return ("toward the E22" if earlier >= 2 and later < 2 else
            "away from the E22" if later >= 2 and earlier < 2 else
            "no change detected" if earlier < 2 and later < 2 else "inconsistent")


def self_check():
    dip = {f: {"diff_us": -60.0, "ci_us": [-90.0, -35.0], "n": [12, 12]} for f in SLICES}
    flat = {f: {"diff_us": 5.0, "ci_us": [-20.0, 25.0], "n": [12, 12]} for f in SLICES}
    assert count(dip, -1) == 5 and judge(dip) == "toward the E22" and judge(flat) == "no change detected"
    print("analyze_knobs: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="*")
    ap.add_argument("--control", default="ctl")
    ap.add_argument("--arms", nargs="+", default=["txm", "ldo", "cal"])
    ap.add_argument("--ref", default="e22")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    ref = at_slices(analyse(args.captures, a=args.control, b=args.ref))
    out = {"positive_control": {"points": ref, "slices_with_lead": count(ref, -1)}}
    out["valid"] = out["positive_control"]["slices_with_lead"] >= 2
    for arm in args.arms:
        pts = at_slices(analyse(args.captures, a=args.control, b=arm))
        out[arm] = {"points": pts, "verdict": judge(pts) if out["valid"] else "invalid run"}
    for name, v in out.items():
        if isinstance(v, dict) and "points" in v:
            print(name, v.get("verdict", f"lead at {v.get('slices_with_lead')} slices"))
            for f, p in sorted(v["points"].items()):
                print(f"  {f:+4d} kHz " + (f"{p['diff_us']:+6.1f} ci {p['ci_us']} n={p['n']}" if p else "no landmark"))
    print(json.dumps({k: (v["verdict"] if isinstance(v, dict) and "verdict" in v else v)
                      for k, v in out.items() if k != "positive_control"}))


if __name__ == "__main__":
    sys.exit(main())
