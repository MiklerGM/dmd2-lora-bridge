#!/usr/bin/env python3
"""
The difference between our frequency path and the E22's as a function of time after a symbol's
opening jump. Rules in docs/PHY-notes.md (2026-09-24), declared before the capture.

Two header symbols are followed: h6 (v = 553, starts at -115.0 kHz, crosses at 17.98 symbols in the
aligned frame whenever the slice is above its start) and h7 (v = 1373, +85.2 kHz, 18.58). For each
scope offset, the landmark within 0.12 symbol of that position is taken from
analyze_scope_timing.analyse(); time after the jump is (f - f0) / 500 kHz x 4096 us.

  bench/analyze_jump.py capture.json.gz
"""

import json
import sys

from analyze_scope_timing import analyse

SYMBOLS = {"h6": (553, 17.98), "h7": (1373, 18.58)}


def start_khz(v):
    return -250.0 + v * 500.0 / 2048


def curve(result):
    out = {name: [] for name in SYMBOLS}
    for o in result:
        f = o["offset_khz"]
        for name, (v, pos) in SYMBOLS.items():
            f0 = start_khz(v)
            if not 0 < f - f0 < 250:
                continue
            near = [r for r in o["landmarks"] if abs(r["symbol"] - pos) <= 0.12]
            if not near:
                out[name].append({"offset_khz": f, "tau_us": round((f - f0) / 500 * 4096), "found": False})
                continue
            r = min(near, key=lambda r: abs(r["symbol"] - pos))
            out[name].append({"offset_khz": f, "tau_us": round((f - f0) / 500 * 4096), "found": True,
                              "diff_us": r["e22_minus_heltec_us"], "ci_us": r["ci_us"], "n": r["n"]})
    for name in out:
        out[name].sort(key=lambda p: p["tau_us"])
    return out


def sig(p, sign, size, base):
    """Departs from the symbol's own settled baseline by size in the given direction, interval clear of it."""
    return (p.get("found") and sign * (p["diff_us"] - base) >= size
            and (p["ci_us"][0] > base if sign > 0 else p["ci_us"][1] < base))


def verdict(c):
    """Declared: relative to each symbol's baseline (median difference from 500 us after the jump on;
    0 if none), the transient is confirmed if both symbols show a dip (30 us below, 80-200 us after
    the jump) and a peak (30 us above, 220-400 us), and their late points stay within 20 us of the
    baseline; refuted if neither symbol departs by 30 us, interval clear, anywhere up to 400 us."""
    per = {}
    for name, pts in c.items():
        late_pts = [p["diff_us"] for p in pts if p.get("found") and p["tau_us"] >= 500]
        base = float(sorted(late_pts)[len(late_pts) // 2]) if late_pts else 0.0
        dip = any(sig(p, -1, 30, base) for p in pts if 80 <= p["tau_us"] <= 200)
        peak = any(sig(p, +1, 30, base) for p in pts if 220 <= p["tau_us"] <= 400)
        late = all(abs(d - base) <= 20 for d in late_pts)
        early = any(sig(p, -1, 30, base) or sig(p, +1, 30, base) for p in pts if p["tau_us"] <= 400)
        per[name] = {"baseline_us": base, "dip": dip, "peak": peak, "settled_by_500us": late,
                     "any_early": early}
    if all(v["dip"] and v["peak"] and v["settled_by_500us"] for v in per.values()):
        v = "transient confirmed"
    elif not any(v["any_early"] for v in per.values()):
        v = "no near-jump difference"
    else:
        v = "mixed"
    return {"per_symbol": per, "verdict": v}


def self_check():
    assert abs(start_khz(553) + 115.0) < 0.05 and abs(start_khz(1373) - 85.2) < 0.05
    c = {n: [{"tau_us": 120, "found": True, "diff_us": -50.0, "ci_us": [-70.0, -35.0]},
             {"tau_us": 300, "found": True, "diff_us": 60.0, "ci_us": [35.0, 90.0]},
             {"tau_us": 650, "found": True, "diff_us": 5.0, "ci_us": [-15.0, 20.0]}] for n in SYMBOLS}
    shifted = {n: [dict(p, diff_us=p["diff_us"] + 20, ci_us=[p["ci_us"][0] + 20, p["ci_us"][1] + 20])
                   for p in pts] for n, pts in c.items()}
    assert verdict(shifted)["verdict"] == "transient confirmed"   # a constant offset is not the effect
    assert verdict(c)["verdict"] == "transient confirmed"
    flat = {n: [{"tau_us": t, "found": True, "diff_us": 4.0, "ci_us": [-20.0, 25.0]} for t in (120, 300)]
            for n in SYMBOLS}
    assert verdict(flat)["verdict"] == "no near-jump difference"
    print("analyze_jump: self-check passes")


def main():
    if sys.argv[1:] == ["--self-check"]:
        return self_check()
    c = curve(analyse(sys.argv[1:]))
    for name, pts in c.items():
        print(name)
        for p in pts:
            print(f"  {p['offset_khz']:+4d} kHz  {p['tau_us']:4d} us after jump  " +
                  (f"diff {p['diff_us']:+6.1f} ci {p['ci_us']} n={p['n']}" if p["found"] else "no landmark"))
    print(json.dumps(verdict(c)))


if __name__ == "__main__":
    sys.exit(main())
