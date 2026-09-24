#!/usr/bin/env python3
"""
Start-of-packet timing, ours against the E22's, from narrowband RSSI captures (numpy only).

Rules in docs/PHY-notes.md (2026-09-24), declared before the capture. Alignment as in the
2026-09-22 waveform review: time from the qualifying chain's fitted first preamble pulse, the E22
moved back by its 12 extra preamble symbols at the nominal 4096 us - no fitted period is
extrapolated, so the only alignment error is that of the origin.
Pulses within 250 us join landmarks across captures; a landmark is compared when it holds a pulse
in at least half of each transmitter's captures at that scope offset. For each: E22 minus Heltec,
median difference with a bootstrap 99.5 % interval over captures (many landmarks per offset).

  bench/analyze_scope_timing.py capture.json.gz [more.json.gz] [--sfd 9.3 12.0]
"""

import argparse
import gzip
import json
import sys
from pathlib import Path

import numpy as np

from analyze_rssi import analyze, load_captures

SYMBOL_US = 4096.0
PREAMBLE = {"heltec": 8, "e22": 20}


def pulses(t, y, peak):
    active = np.flatnonzero(y > peak - 8)
    out = []
    for group in np.split(active, np.flatnonzero(np.diff(t[active]) > 250) + 1):
        if len(group) >= 2:
            w = 10 ** ((y[group] - peak) / 10)
            out.append(float(np.sum(t[group] * w) / np.sum(w)))
    return np.array(out)


def origin(chain, tx):
    """Fitted time of our preamble's first symbol; the E22's twelve extra symbols removed."""
    _, t0 = np.polyfit(np.arange(len(chain)), chain, 1)
    return t0 + (PREAMBLE[tx] - PREAMBLE["heltec"]) * SYMBOL_US


def landmarks(caps, lo, hi, reach=150.0):
    """Landmark centres from the pooled pulse histogram, then each capture's nearest pulse.

    Centres are the tallest 100-us-window counts, greedily, each claiming +/-300 us, kept while at
    least half the captures contribute. Chaining pulses into clusters instead lets one capture's
    stray pulses glue two symbols together. Returns [(centre, {capture index: time})].
    """
    pooled = np.array([t for _, ts in caps for t in ts if lo <= t <= hi])
    if not len(pooled):
        return []
    grid = np.arange(lo, hi, 10.0)
    counts = np.array([np.sum(np.abs(pooled - g) <= 50) for g in grid])
    out, taken = [], np.zeros(len(grid), bool)
    while True:
        free = np.where(taken, -1, counts)
        k = int(np.argmax(free))
        if free[k] < len(caps) / 2:
            break
        centre = grid[k]
        taken |= np.abs(grid - centre) <= 300
        per = {}
        for i, (_, ts) in enumerate(caps):
            near = ts[np.abs(ts - centre) <= reach]
            if len(near):
                per[i] = float(near[np.argmin(np.abs(near - centre))])
        out.append((float(centre), per))
    return sorted(out)


CI_LEVEL = 99.5   # a dozen landmarks per offset: a 95 % interval would flag something by chance


def boot_ci(a, b, rng, n=8000):
    d = [np.median(rng.choice(b, len(b))) - np.median(rng.choice(a, len(a))) for _ in range(n)]
    tail = (100 - CI_LEVEL) / 2
    return [float(np.percentile(d, tail)), float(np.percentile(d, 100 - tail))]


def analyse(paths, sfd_window=(9.3, 12.0)):
    # Qualified per file, as each run was qualified on its own quiet windows - and keyed by file
    # too, since every run numbers its packets from 100001.
    caps, qual = [], {}
    for path in paths:
        mine = load_captures([Path(path)])
        caps += mine
        qual.update({(str(Path(path)), r["seq"]): r for r in analyze(mine)["rows"]})
    rng = np.random.default_rng(1)
    by_offset = {}
    for path, attempt, t, y in caps:
        tx = attempt["tx"]
        chain = qual.get((path, attempt["seq"]), {}).get("train_us")
        if not tx or not chain:
            continue
        peak = float(np.percentile(y[(t >= chain[0] - 2000) & (t < chain[-1] + 2000)], 99))
        p = pulses(t, y, peak)
        ref = origin(chain, tx)
        by_offset.setdefault(attempt["capture"]["offset_khz"], []).append((tx, p - ref))
    out = []
    for offset, caps_o in sorted(by_offset.items()):
        n = {tx: sum(c[0] == tx for c in caps_o) for tx in PREAMBLE}
        rows = []
        for _, per in landmarks(caps_o, 6.5 * SYMBOL_US, 21.0 * SYMBOL_US):
            h = np.array([v for i, v in per.items() if caps_o[i][0] == "heltec"])
            e = np.array([v for i, v in per.items() if caps_o[i][0] == "e22"])
            if len(h) < max(2, n["heltec"] / 2) or len(e) < max(2, n["e22"] / 2):
                continue
            sym = float(np.median(np.r_[h, e])) / SYMBOL_US
            rows.append({"symbol": round(sym, 2),
                         "region": "sfd" if sfd_window[0] <= sym <= sfd_window[1] else "upchirp",
                         "n": [len(h), len(e)],
                         "e22_minus_heltec_us": round(float(np.median(e) - np.median(h)), 1),
                         "ci_us": [round(v, 1) for v in boot_ci(h, e, rng)]})
        sfd = [r["e22_minus_heltec_us"] for r in rows if r["region"] == "sfd"]
        out.append({"offset_khz": offset, "captures": n, "sfd_median_shift_us": float(np.median(sfd)) if sfd else None,
                    "header_shift": header_shift(caps_o, rng), "landmarks": rows})
    return out


def header_shift(caps, rng, lo=12.2 * SYMBOL_US, hi=21.0 * SYMBOL_US):
    """Whole-header displacement: per capture, the mean deviation of its header-region pulses from
    the landmark centres; then E22 minus Heltec, with a bootstrap interval."""
    marks = [(c, per) for c, per in landmarks(caps, lo, hi)]
    if not marks:
        return None
    dev = {}
    for i in range(len(caps)):
        d = [per[i] - c for c, per in marks if i in per]
        if len(d) >= len(marks) / 2:
            dev[i] = float(np.mean(d))
    h = np.array([v for i, v in dev.items() if caps[i][0] == "heltec"])
    e = np.array([v for i, v in dev.items() if caps[i][0] == "e22"])
    if len(h) < 2 or len(e) < 2:
        return None
    return {"e22_minus_heltec_us": round(float(np.median(e) - np.median(h)), 1),
            "ci_us": [round(v, 1) for v in boot_ci(h, e, rng)], "n": [len(h), len(e)]}


# Local differences seen in the 2026-09-22 captures (exploratory), to be replicated or not:
# (scope offset kHz, landmark symbol, sign of E22 minus Heltec).
TARGETS = [(-100, 17.98, -1), (0, 14.44, +1), (100, 18.59, -1)]


def replicated(result, targets=TARGETS, min_us=20.0):
    out = []
    for off, sym, sign in targets:
        o = next((x for x in result if x["offset_khz"] == off), None)
        hit = None
        for r in (o["landmarks"] if o else []):
            if abs(r["symbol"] - sym) <= 0.1:
                hit = r
        ok = bool(hit and np.sign(hit["e22_minus_heltec_us"]) == sign
                  and abs(hit["e22_minus_heltec_us"]) >= min_us
                  and (hit["ci_us"][0] > 0 or hit["ci_us"][1] < 0))
        out.append({"offset_khz": off, "symbol": sym, "found": hit, "replicated": ok})
    return out


def verdict(result, shift_us=10.0, sfd_us=15.0):
    """The declared rules: a whole-header shift must appear at two or more scope offsets."""
    up = sum(bool(o.get("header_shift")) and abs(o["header_shift"]["e22_minus_heltec_us"]) >= shift_us
             and (o["header_shift"]["ci_us"][0] > 0 or o["header_shift"]["ci_us"][1] < 0) for o in result)
    sf = 0
    for o in result:
        m = o["sfd_median_shift_us"]
        if m is None:
            continue
        sf += any(r["region"] == "sfd" and abs(r["e22_minus_heltec_us"] - m) >= sfd_us
                  and not (r["ci_us"][0] <= m <= r["ci_us"][1]) for r in o["landmarks"])
    return {"header_shift_offsets_flagged": up, "sfd_offsets_flagged": sf,
            "verdict": ("the header is displaced" if up >= 2 else
                        "SFD structure differs" if sf >= 2 else
                        "no header or SFD displacement at this resolution"),
            "local": replicated(result)}


def self_check():
    chain = [1000 + 4096 * k for k in range(6)]
    assert abs(origin(chain, "heltec") - 1000) < 1e-6
    assert abs(origin(chain, "e22") - (1000 + 12 * 4096)) < 1e-6
    rows = [{"offset_khz": 0, "sfd_median_shift_us": -130.0,
             "header_shift": {"e22_minus_heltec_us": 12.0, "ci_us": [3.0, 20.0]}, "landmarks": [
        {"symbol": 14.47, "region": "upchirp", "e22_minus_heltec_us": 30.0, "ci_us": [5.0, 50.0]},
        {"symbol": 10.0, "region": "sfd", "e22_minus_heltec_us": -130.0, "ci_us": [-140.0, -120.0]}]}]
    v = verdict(rows)
    assert v["header_shift_offsets_flagged"] == 1 and v["local"][1]["replicated"]
    assert not v["local"][0]["replicated"]
    assert verdict(rows * 2)["verdict"] == "the header is displaced"
    print("analyze_scope_timing: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("captures", nargs="*")
    ap.add_argument("--sfd", type=float, nargs=2, default=[9.3, 12.0],
                    help="symbols after our first preamble symbol counted as the SFD window")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    result = analyse(args.captures, args.sfd)
    for o in result:
        print(f"offset {o['offset_khz']:+d} kHz captures {o['captures']} sfd shift {o['sfd_median_shift_us']} "
              f"header shift {o['header_shift']}")
        for r in o["landmarks"]:
            print(f"  {r['symbol']:6.2f} {r['region']:8s} n={r['n']} "
                  f"diff {r['e22_minus_heltec_us']:+7.1f} ci {r['ci_us']}")
    print(json.dumps(verdict(result)))


if __name__ == "__main__":
    sys.exit(main())
