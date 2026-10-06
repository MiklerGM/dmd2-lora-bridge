#!/usr/bin/env python3
"""
Where the T-Deck's failed receptions of our packets sit against its successful ones: frequency
estimate (RegFreqError), SNR and RSSI read at the failure. Rules in docs/PHY-notes.md
(2026-09-24), declared before the run.

A failure line carries no sequence number, so it is given to the packet whose transmission
started last before it, within 2 s.

  bench/analyze_fail.py run.json [--arm heltec] [--ref e22] [--rx tdeck]
  bench/analyze_fail.py --self-check
"""

import argparse
import json
import random
import re
import statistics as st
import sys

FAIL = re.compile(r"rx fail (hdr|crc) rssi (-?\d+) dBm, snr (-?\d+\.\d+) dB.*preamble->fail (-?\d+) ms")
QUALITY = re.compile(r"rx quality rssi (-?\d+) dBm, snr (-?\d+\.\d+) dB")
FREQ = re.compile(r"sx1262: (fail )?freq error ([+-]?\d+) Hz")
RX = re.compile(r"bridge: rx \d+ bytes(?:, rssi -?\d+)?: (\d{8})")


def parse(lines, times, starts):
    """(successes {seq: {rssi, snr, fe}}, failures [{seq, kind, rssi, snr, ms, fe, prev_fe}])."""
    ok, fails, cur, prev_fe = {}, [], {}, None
    order = sorted(starts.items(), key=lambda kv: kv[1])
    for line, t in zip(lines, times):
        m = FAIL.search(line)
        if m:
            seq = None
            for s, t0 in order:
                if t0 > t:
                    break
                seq = s if t - t0 <= 2.0 else None
            fails.append({"seq": seq, "kind": m[1], "rssi": int(m[2]), "snr": float(m[3]),
                          "ms": int(m[4]), "fe": None, "prev_fe": prev_fe})
            continue
        m = FREQ.search(line)
        if m:
            if m[1]:
                if fails:
                    fails[-1]["fe"] = int(m[2])
            else:
                cur["fe"] = int(m[2])
            prev_fe = int(m[2])
            continue
        m = QUALITY.search(line)
        if m:
            cur = {"rssi": int(m[1]), "snr": float(m[2])}
            continue
        m = RX.search(line)
        if m:
            if cur:
                ok[int(m[1])] = cur
            cur = {}
    return ok, fails


def median_diff_ci(a, b, n=10000, seed=1):
    """median(a) - median(b) with a 99.5 % bootstrap interval."""
    rng = random.Random(seed)
    d = sorted(st.median(rng.choices(a, k=len(a))) - st.median(rng.choices(b, k=len(b))) for _ in range(n))
    return st.median(a) - st.median(b), (d[int(0.0025 * n)], d[int(0.9975 * n) - 1])


def level_verdict(diff, ci, margin):
    if diff >= margin and ci[0] > 0:
        return "failures lower"
    if ci[0] > -margin and ci[1] < margin:
        return "no difference"
    return "inconclusive"


def analyse(run, arm, ref, rx):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {int(x) for x in run["seen"][rx]}
    starts = {int(k): v[0] for k, v in run["tx_times"].items()}
    ok, fails = parse(run["log"][rx], run["log_t"][rx], starts)
    arm_seq = [s for s, w in sent.items() if w == arm]
    ref_seq = [s for s, w in sent.items() if w == ref]
    misses = [s for s in arm_seq if s not in seen]
    f_arm = [f for f in fails if sent.get(f["seq"]) == arm]
    f_fe = [f for f in f_arm if f["fe"] is not None]
    s_arm = [ok[s] for s in arm_seq if s in ok and "fe" in ok[s]]
    arm_fe_median = st.median(x["fe"] for x in s_arm)
    stale = [f for f in f_fe if f["prev_fe"] is not None and f["fe"] == f["prev_fe"]
             and abs(f["prev_fe"] - arm_fe_median) > 200]
    foreign = [f for f in f_fe if f["prev_fe"] is not None and abs(f["prev_fe"] - arm_fe_median) > 200]
    out = {
        "delivered": {arm: f"{len(arm_seq) - len(misses)}/{len(arm_seq)}",
                      ref: f"{sum(s in seen for s in ref_seq)}/{len(ref_seq)}"},
        "failures_by_arm": {w: sum(sent.get(f["seq"]) == w for f in fails) for w in set(sent.values())},
        "unattributed_failures": sum(f["seq"] is None for f in fails),
        "arm_misses_with_fail_line": f"{len({f['seq'] for f in f_arm} & set(misses))}/{len(misses)}",
        "arm_failures_with_freq": len(f_fe),
        "stale_after_other_transmitter": f"{len(stale)}/{len(foreign)}",
        "fail_kinds": {k: sum(f["kind"] == k for f in f_arm) for k in ("hdr", "crc")},
    }
    valid = (sum(s in seen for s in ref_seq) >= 0.9 * len(ref_seq)
             and len({f["seq"] for f in f_arm} & set(misses)) >= 0.8 * len(misses)
             and len(f_fe) >= 20 and (not foreign or len(stale) <= len(foreign) / 2))
    out["valid"] = valid
    succ_fe = sorted(x["fe"] for x in s_arm)
    lo, hi = succ_fe[int(0.01 * len(succ_fe))], succ_fe[-int(0.01 * len(succ_fe)) - 1]
    outside = sum(not lo <= f["fe"] <= hi for f in f_fe) / len(f_fe) if f_fe else None
    out["freq"] = {"successes_p1_p99_hz": [lo, hi], "successes_median_hz": arm_fe_median,
                   "failures_hz": sorted(f["fe"] for f in f_fe), "failures_outside": outside,
                   "verdict": ("invalid" if not valid else
                               "failures sit at a different frequency estimate" if outside >= 0.5 else
                               "failures among the successes" if outside <= 0.1 else "mixed")}
    for key, margin in (("snr", 1.0), ("rssi", 2.0)):
        a, b = [x[key] for x in s_arm], [f[key] for f in f_arm]
        diff, ci = median_diff_ci(a, b)
        out[key] = {"success_median": st.median(a), "failure_median": st.median(b),
                    "success_minus_failure": round(diff, 2), "ci995": [round(ci[0], 2), round(ci[1], 2)],
                    "failures": sorted(b),
                    "verdict": "invalid" if not valid else level_verdict(diff, ci, margin)}
    out["preamble_to_fail_ms"] = sorted(f["ms"] for f in f_arm)
    ref_fe = sorted(ok[s]["fe"] for s in ref_seq if s in ok and "fe" in ok[s])
    out["ref_freq_p1_median_p99"] = ([ref_fe[int(0.01 * len(ref_fe))], st.median(ref_fe),
                                      ref_fe[-int(0.01 * len(ref_fe)) - 1]] if ref_fe else None)
    return out


def self_check():
    lines = ["sx1262: rx quality rssi -55 dBm, snr 7.50 dB, signal rssi -56 dBm",
             "sx1262: freq error -13 Hz (raw 27)",
             "bridge: rx 38 bytes: 00000001AAAA",
             "sx1262: rx quality rssi -60 dBm, snr 8.00 dB, signal rssi -60 dBm",
             "sx1262: freq error +8577 Hz (raw 17720)",
             "bridge: rx 38 bytes: 00000002AAAA",
             "sx1262: rx fail hdr rssi -56 dBm, snr -1.25 dB, signal rssi -58 dBm, preamble->fail 80 ms",
             "sx1262: fail freq error +8577 Hz (raw 17720)"]
    times = [1.1, 1.1, 1.2, 2.1, 2.1, 2.2, 3.1, 3.1]
    ok, fails = parse(lines, times, {1: 1.0, 2: 2.0, 3: 3.0})
    assert ok == {1: {"rssi": -55, "snr": 7.5, "fe": -13}, 2: {"rssi": -60, "snr": 8.0, "fe": 8577}}
    assert fails == [{"seq": 3, "kind": "hdr", "rssi": -56, "snr": -1.25, "ms": 80, "fe": 8577,
                      "prev_fe": 8577}]
    _, fails = parse(lines, times, {1: 0.2, 2: 0.5, 3: 0.8})
    assert fails[0]["seq"] is None   # nothing started within 2 s before it
    diff, ci = median_diff_ci([8.0] * 30, [2.0] * 30)
    assert diff == 6.0 and ci == (6.0, 6.0) and level_verdict(diff, ci, 1.0) == "failures lower"
    assert level_verdict(0.2, (-0.5, 0.6), 1.0) == "no difference"
    print("analyze_fail: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--arm", default="heltec")
    ap.add_argument("--ref", default="e22")
    ap.add_argument("--rx", default="tdeck")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    print(json.dumps(analyse(json.load(open(args.run)), args.arm, args.ref, args.rx), indent=1))


if __name__ == "__main__":
    sys.exit(main())
