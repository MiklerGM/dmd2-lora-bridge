#!/usr/bin/env python3
"""
Classify each packet of an `interleave.py --probe` run by what the E220 said about it.

Implements the rules declared in docs/PHY-notes.md (2026-09-23) before the run: a packet's
window in the E220 log runs from the previous query reply to its own; register 1 ("last
received RSSI") is compared with the medians of delivered reference and delivered G packets;
AUX pulses that start more than 400 ms before the window's reply, and UART chunks, count as
the module having output something.

  bench/analyze_register.py run.json [--rx e220] [--ref ref] [--good G] [--table]
"""

import argparse
import json
import re
import statistics
import sys

RSSIQ = re.compile(r"RSSIQ t=(\d+) got=(-?\d+) raw=(\w*) noise=(-?\d+) last=(-?\d+)")
AUX = re.compile(r"aux low t=(\d+) us=(\d+)")
UART = re.compile(r"e220: uart t=(\d+) n=(\d+) (\w*)")
QUERY_PULSE_US = 400_000


def windows(lines, replies):
    """seq -> (reply fields, [lines in its window]). replies: seq -> reply line text."""
    at = {line: i for i, line in enumerate(lines) if "RSSIQ" in line}
    out, start = {}, 0
    for seq in sorted(replies, key=int):
        line = replies[seq]
        if line is None or line not in at:
            out[int(seq)] = (None, [])
            continue
        i = at[line]
        m = RSSIQ.search(line)
        out[int(seq)] = ({"t": int(m[1]), "got": int(m[2]), "raw": m[3], "noise": int(m[4]),
                          "last": int(m[5])}, lines[start:i])
        start = i + 1
    return out


def classify(run, rx="e220", ref="ref", good="G"):
    sent = {int(k): v for k, v in run["sent"].items()}
    seen = {k: {int(x) for x in v} for k, v in run["seen"].items()}
    replies = {k: v.get(rx) for k, v in run["probe"].items()}
    per = {}
    for seq, (reply, win) in windows(run["log"][rx], replies).items():
        if seq not in sent:
            continue
        t_q = reply["t"] if reply else None
        pulses = [(int(m[1]), int(m[2])) for m in map(AUX.search, win) if m]
        rx_pulses = [p for p in pulses if t_q is not None and p[0] < t_q - QUERY_PULSE_US]
        chunks = [(int(m[2]), m[3]) for m in map(UART.search, win) if m]
        own = f"{seq:08d}".encode().hex().upper()
        per[seq] = {
            "arm": sent[seq],
            "delivered": seq in seen[rx],
            "others": {k: seq in v for k, v in seen.items() if k != rx},
            "reply_ok": bool(reply) and reply["got"] == 5 and reply["raw"].startswith("C10002"),
            "last": reply["last"] if reply else None,
            "noise": reply["noise"] if reply else None,
            "rx_pulses": rx_pulses,
            "chunks": chunks,
            # A chunk that does not carry this packet's sequence number is someone else's output.
            "foreign": [c for c in chunks if own not in c[1]],
        }

    def med(arm):
        vals = [p["last"] for p in per.values() if p["arm"] == arm and p["delivered"] and p["reply_ok"]]
        return statistics.median(vals) if vals else None

    l_e, l_h = med(ref), med(good)
    mid = (l_e + l_h) / 2 if l_e is not None and l_h is not None else None
    for seq, p in per.items():
        p["registered"] = None
        if mid is None or not p["reply_ok"]:
            continue
        # Nearer the Heltec level than the E22 level: register 1 moved to this packet.
        p["registered"] = (p["last"] < mid) if l_h < l_e else (p["last"] > mid)
    for seq, p in per.items():
        prev = per.get(seq - 1)
        p["classifiable"] = (p["arm"] != ref and prev is not None and prev["arm"] == ref
                             and prev["delivered"] and not p["foreign"] and p["registered"] is not None)
    return per, l_e, l_h


def summary(per, l_e, l_h, ref="ref", good="G"):
    arms = sorted({p["arm"] for p in per.values()})
    out = {"L_E": l_e, "L_H": l_h, "arms": {}}
    for arm in arms:
        ps = [p for p in per.values() if p["arm"] == arm]
        cl = [p for p in ps if p["classifiable"]]
        lost = [p for p in cl if not p["delivered"]]
        out["arms"][arm] = {
            "sent": len(ps),
            "delivered": sum(p["delivered"] for p in ps),
            "reply_ok": sum(p["reply_ok"] for p in ps),
            "classifiable": len(cl),
            "registered_all": sum(bool(p["registered"]) for p in cl),
            "lost_classifiable": len(lost),
            "registered_lost": sum(bool(p["registered"]) for p in lost),
            "lost_with_output": sum(bool(p["rx_pulses"] or p["chunks"])
                                    for p in ps if not p["delivered"]),
            "delivered_with_pulse_and_chunk": sum(bool(p["rx_pulses"] and p["chunks"])
                                                  for p in ps if p["delivered"]),
        }
    # Own-source classification of delivered packets: the validity check on the level split.
    own = [p for p in per.values() if p["delivered"] and p["arm"] in (ref, good)
           and p["registered"] is not None]
    right = sum((p["arm"] == good) == p["registered"] for p in own)
    out["delivered_own_source"] = f"{right}/{len(own)}"
    return out


def self_check():
    lines = ["boot", "e220: aux low t=1000000 us=40000", "e220: uart t=1100000 n=38 3030303030303031",
             "e220: aux low t=1560000 us=9000", "e220: RSSIQ t=1600000 got=5 raw=C10002B0D6 noise=-80 last=-42",
             "e220: aux low t=3560000 us=9000", "e220: RSSIQ t=3600000 got=5 raw=C10002B0CE noise=-80 last=-50"]
    run = {"sent": {"1": "ref", "2": "G"}, "seen": {"e220": [1], "tdeck": [1, 2]},
           "probe": {"1": {"e220": lines[4]}, "2": {"e220": lines[6]}}, "log": {"e220": lines}}
    per, l_e, l_h = classify(run)
    assert per[1]["delivered"] and per[1]["rx_pulses"] == [(1000000, 40000)] and not per[1]["foreign"]
    assert not per[2]["delivered"] and per[2]["rx_pulses"] == [] and per[2]["chunks"] == []
    assert l_e == -42 and l_h is None and per[2]["registered"] is None   # no delivered G yet
    print("analyze_register: self-check passes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", nargs="?")
    ap.add_argument("--rx", default="e220")
    ap.add_argument("--ref", default="ref")
    ap.add_argument("--good", default="G")
    ap.add_argument("--table", action="store_true", help="print every packet")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    run = json.load(open(args.run))
    per, l_e, l_h = classify(run, args.rx, args.ref, args.good)
    if args.table:
        for seq in sorted(per):
            p = per[seq]
            print(f"{seq:4d} {p['arm']:4s} dlv={int(p['delivered'])} oth={''.join(str(int(v)) for v in p['others'].values())}"
                  f" last={p['last']} noise={p['noise']} reg={p['registered']} cls={int(p['classifiable'])}"
                  f" pulses={[w for _, w in p['rx_pulses']]} chunks={[n for n, _ in p['chunks']]}"
                  f"{' FOREIGN' if p['foreign'] else ''}")
    print(json.dumps(summary(per, l_e, l_h, args.ref, args.good), indent=1))


if __name__ == "__main__":
    sys.exit(main())
