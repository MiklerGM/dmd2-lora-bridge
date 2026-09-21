#!/usr/bin/env python3
"""
Send numbered packets from several transmitters in turn and count which receivers heard them.

Why interleaved rather than one transmitter after the other: the link wanders on its own. Over
a bench run the decode probability drifts with a standard deviation around 0.10 and a time
constant near two minutes, which is larger than most of the effects worth measuring. Two
separate runs of the same thing can differ by more than a real difference between two different
things, and this bench has already produced three findings that evaporated on a re-run.

Interleaving turns drift into common mode: whatever the link is doing at 14:32 it is doing to
every transmitter at 14:32. Rounds are ordered ABBA rather than ABAB so that a linear trend
cancels within each round instead of accumulating against whichever transmitter goes first.

Each transmitter needs the BENCH_CMD build; it answers "TXOK seq=N" once the packet is off the
air, so a packet that never left is not counted as one the receiver lost. Each receiver needs
the ordinary bridge build, which logs every packet it decodes. The sequence number leads the
payload, so a reception names its transmission outright and nothing is matched by timing.

  bench/interleave.py --n 100 \
      --tx tdeck=/dev/ttyACM0 --tx heltec=/dev/ttyACM1 \
      --rx e220=pi:/dev/ttyUSB0 --rx e22=pi:/dev/ttyUSB1

A receiver is either a local device path or host:/dev/... reached over ssh.
"""

import argparse
import base64
import itertools
import json
import re
import subprocess
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path

# Runs on the receiving machine, fed in over ssh so nothing has to be installed there beyond
# pyserial. Unbuffered, because a line that arrives after the run has been counted is lost.
REMOTE_NODE = """
import sys, time, threading, serial
port = serial.Serial(sys.argv[1], 115200, timeout=0.5)
port.dtr = False
port.rts = True
time.sleep(0.15)
port.rts = False

def to_port():
    # readline, not iteration: iterating a pipe reads ahead and holds a line back until the
    # buffer fills, so a command sent now would leave minutes later, or never.
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            break
        port.write(line)
        port.flush()

threading.Thread(target=to_port, daemon=True).start()
while True:
    line = port.readline()
    if line:
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()
"""

REMOTE_READER = """
import sys, time, serial
port = serial.Serial(sys.argv[1], 115200, timeout=0.5)
port.dtr = False
port.rts = True
time.sleep(0.15)
port.rts = False
while True:
    line = port.readline()
    if line:
        sys.stdout.buffer.write(line)
        sys.stdout.buffer.flush()
"""

TXOK = re.compile(r"TXOK seq=(\d+)")
TXFAIL = re.compile(r"TXFAIL seq=(\d+)")
# The bridge logs "rx 38 bytes: 00000042AAAA..." - the payload starts with its sequence number.
RX = re.compile(r"rx \d+ bytes: (\d{8})")


def reset(port):
    """Pulse the board's reset line: DTR low so it boots the application, then a dip on RTS.

    Every run starts from a fresh receiver, and the point is not hygiene but evidence. A board
    that has quietly stopped reporting looks exactly like one that heard nothing, and this bench
    has now lost an hour to that twice. A reset puts the boot banner at the top of the captured
    log, so afterwards there is a line in the record saying the receiver was alive and which
    module it was carrying.
    """
    port.dtr = False
    port.rts = True
    time.sleep(0.15)
    port.rts = False


class Node(threading.Thread):
    """One board, one connection, both roles.

    A matrix needs every board to transmit in its turn and listen the rest of the time, and two
    connections to one serial port fight over it - which this bench has already lost an hour to,
    twice. So a node owns its port, a reader thread collects everything the board says, and
    transmitting is writing a line and waiting for that thread to see the acknowledgement.
    """

    daemon = True

    def __init__(self, name, spec):
        super().__init__()
        self.name, self.spec = name, spec
        self.seen, self.lines = {}, []
        self.stop = threading.Event()
        self.proc, self.port = None, None
        self.lock = threading.Lock()
        if ":" in spec:
            host, dev = spec.split(":", 1)
            # The script is placed as a file first. Feeding it on stdin - which is how the
            # read-only reader does it - hands the interpreter our only channel to the board:
            # python consumes stdin as its program text and closes it, and every command written
            # afterwards goes into a pipe nothing is reading.
            script = base64.b64encode(REMOTE_NODE.encode()).decode()
            subprocess.run(["ssh", host, f"echo {script} | base64 -d > /tmp/bench_node.py"],
                           check=True, stdout=subprocess.DEVNULL)
            self.proc = subprocess.Popen(
                ["ssh", host, f"python3 -u /tmp/bench_node.py {dev}"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        else:
            import serial
            self.port = serial.Serial(spec, 115200, timeout=0.5)
            reset(self.port)
        self.start()

    def run(self):
        stream = self.proc.stdout if self.proc else self.port
        while not self.stop.is_set():
            try:
                raw = stream.readline()
            except Exception:
                break
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            with self.lock:
                self.lines.append(line)
            m = RX.search(line)
            if m:
                self.seen.setdefault(int(m.group(1)), time.time())

    def write(self, text):
        data = text.encode()
        if self.proc:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        else:
            self.port.write(data)
            self.port.flush()

    def send(self, seq, extra, timeout=12.0):
        """True once the board says the packet left. A refusal is not a lost packet."""
        with self.lock:
            mark = len(self.lines)
        self.write(f"TX seq={seq} {extra}\n")
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                fresh = self.lines[mark:]
            for line in fresh:
                if TXOK.search(line) and str(seq) in line:
                    return True
                if TXFAIL.search(line) or "TXERR" in line:
                    print(f"  ! {self.name} refused seq={seq}: {line.strip()}")
                    return False
            time.sleep(0.05)
        print(f"  ! {self.name} never answered for seq={seq}")
        return False

    def close(self):
        self.stop.set()
        if self.proc:
            self.proc.terminate()
        self.join(timeout=3.0)
        if self.port:
            try:
                self.port.close()
            except Exception:
                pass


class Receiver(threading.Thread):
    """Reads one receiver's console and records the sequence number of everything it decodes."""

    daemon = True

    def __init__(self, name, spec):
        super().__init__()
        self.name, self.spec = name, spec
        self.seen = {}          # seq -> arrival time
        self.lines = []
        self.stop = threading.Event()
        self.proc = None
        self.port = None

    def _open(self):
        if ":" in self.spec:
            host, dev = self.spec.split(":", 1)
            # pyserial on the far end rather than stty and cat: cat delivered almost nothing
            # here, and a reader that silently returns no bytes is indistinguishable from a
            # receiver that heard nothing - which is the one confusion this bench cannot
            # afford. Opening the port resets the board, so its boot banner lands at the top
            # of the captured log and says out loud that the receiver is alive and which
            # module it is.
            script = base64.b64encode(REMOTE_READER.encode()).decode()
            # -tt so that killing ssh takes the remote reader with it instead of leaving it
            # holding the port.
            self.proc = subprocess.Popen(
                ["ssh", "-tt", host,
                 f"echo {script} | base64 -d | python3 -u - {dev}"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            return self.proc.stdout
        import serial
        self.port = serial.Serial(self.spec, 115200, timeout=0.5)
        reset(self.port)
        return self.port

    def run(self):
        stream = self._open()
        while not self.stop.is_set():
            try:
                raw = stream.readline()
            except Exception:
                break          # the port went away, which on the way out is expected
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            self.lines.append(line)
            m = RX.search(line)
            if m:
                self.seen.setdefault(int(m.group(1)), time.time())

    def close(self):
        """Stop first and wait, then close: closing the port under a blocked reader raises."""
        self.stop.set()
        if self.proc:
            self.proc.terminate()
        self.join(timeout=3.0)
        if self.port:
            try:
                self.port.close()
            except Exception:
                pass


class Transmitter:
    def __init__(self, name, path):
        import serial
        self.name, self.path = name, path
        self.port = serial.Serial(path, 115200, timeout=0.2)
        # Same reason as the receivers, plus one of its own: a board left in the bootloader by
        # the flash that put this firmware on it answers nothing, and a run where nothing was
        # transmitted otherwise prints a tidy table of zeroes.
        reset(self.port)
        time.sleep(2.5)
        self.port.reset_input_buffer()

    def send(self, seq, extra, timeout=8.0):
        """Returns True once the board reports the packet left. A refusal is not a lost packet."""
        self.port.write(f"TX seq={seq} {extra}\n".encode())
        self.port.flush()
        end, buf = time.time() + timeout, ""
        while time.time() < end:
            buf += self.port.read(4096).decode("utf-8", "replace")
            if TXOK.search(buf) and str(seq) in buf:
                return True
            if TXFAIL.search(buf) or "TXERR" in buf:
                print(f"  ! {self.name} refused seq={seq}: {buf.strip().splitlines()[-1:]}")
                return False
        print(f"  ! {self.name} never answered for seq={seq}")
        return False


def build_schedule(names, n):
    """The order to transmit in: ABBA rounds, exactly n packets per transmitter."""
    out, quota = [], defaultdict(int)
    while any(quota[x] < n for x in names):
        for name in names + list(reversed(names)):
            if quota[name] < n:
                out.append(name)
                quota[name] += 1
    return out


def self_check():
    """Runs without a board: the scheduler is the only part here that can be wrong quietly."""
    for n in (1, 2, 3, 7, 100):
        for names in (["a", "b"], ["a", "b", "c"]):
            s = build_schedule(names, n)
            assert len(s) == n * len(names), (n, names, len(s))
            for x in names:
                assert s.count(x) == n, (n, names, x, s.count(x))
    # ABBA, not ABAB: the second half of a full round mirrors the first.
    assert build_schedule(["a", "b"], 2) == ["a", "b", "b", "a"]
    assert build_schedule(["a", "b"], 1) == ["a", "b"]
    # An odd quota must not overshoot just because rounds come in pairs.
    assert build_schedule(["a", "b"], 3) == ["a", "b", "b", "a", "a", "b"]
    print("schedule: all checks pass")


def matrix(args):
    """Every board transmits in its turn; every other board listens. One run, all pairings.

    Measuring pairings one at a time cannot be compared: the link drifts by more between two
    runs than most of the differences worth seeing. Here every pairing meets the same air in
    the same minutes, and a row can be read against its neighbours honestly.
    """
    nodes = [Node(*spec.split("=", 1)) for spec in args.node]
    print(f"{len(nodes)} boards, {args.n} packets each, every pairing in one run")
    time.sleep(6.0)   # ssh sessions up, and any board that reset on open finished booting

    by_name = {n.name: n for n in nodes}
    schedule = [by_name[x] for x in build_schedule([n.name for n in nodes], args.n)]
    sent, counter = {}, itertools.count(1)
    for i, tx in enumerate(schedule, 1):
        seq = next(counter)
        if tx.send(seq, args.extra):
            sent[seq] = tx.name
        print(f"\r{i}/{len(schedule)} packets, {len(sent)} away ", end="", flush=True)
        time.sleep(args.gap)
    print()

    print(f"waiting {args.settle}s for the last receptions")
    time.sleep(args.settle)
    for n in nodes:
        n.close()

    table = defaultdict(lambda: defaultdict(int))
    totals = defaultdict(int)
    for seq, who in sent.items():
        totals[who] += 1
        for n in nodes:
            if seq in n.seen:
                table[who][n.name] += 1

    w = max(len(n.name) for n in nodes) + 2
    print("\nshare decoded: rows transmit, columns listen")
    print(" " * w + "".join(f"{n.name:>14}" for n in nodes) + "     sent")
    for tx in nodes:
        row = ""
        for rx in nodes:
            if rx is tx:
                row += f"{'-':>14}"     # a board does not hear itself
                continue
            got, total = table[tx.name][rx.name], totals[tx.name]
            row += f"{got:>5}/{total:<3}{(100.0 * got / total if total else 0):>3.0f}%"
        print(f"{tx.name:<{w}}{row}{totals[tx.name]:>9}")

    if args.out:
        args.out.write_text(json.dumps({
            "sent": sent,
            "seen": {n.name: sorted(n.seen) for n in nodes},
            "log": {n.name: n.lines for n in nodes},
        }, indent=1))
        print(f"\nraw result in {args.out}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tx", action="append", metavar="NAME=PORT")
    ap.add_argument("--rx", action="append", metavar="NAME=[HOST:]PORT")
    ap.add_argument("--n", type=int, default=100, help="packets per transmitter")
    ap.add_argument("--extra", default="", help="extra TX tokens, e.g. 'power=0 len=64'")
    ap.add_argument("--gap", type=float, default=0.4, help="seconds between packets")
    ap.add_argument("--settle", type=float, default=35.0,
                    help="seconds to wait at the end for the last packets and for a receiver "
                         "to print its cumulative counters after them")
    ap.add_argument("--out", type=Path, help="write the raw result here as JSON")
    ap.add_argument("--node", action="append", metavar="NAME=[HOST:]PORT",
                    help="a board that both transmits in its turn and listens the rest of the "
                         "time; give several and every pairing is measured in one run")
    ap.add_argument("--self-check", action="store_true",
                    help="check the scheduler and exit; needs no hardware")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    if args.node:
        return matrix(args)
    if not args.tx or not args.rx:
        ap.error("at least one --tx and one --rx are required, or use --node")

    txs = [Transmitter(*s.split("=", 1)) for s in args.tx]
    rxs = [Receiver(*s.split("=", 1)) for s in args.rx]
    for r in rxs:
        r.start()
    # Long enough for an ssh session to come up and for a board that reset when its port was
    # opened to finish booting. A receiver still booting is a receiver that misses packets.
    time.sleep(6.0)

    print(f"{len(txs)} transmitters, {len(rxs)} receivers, {args.n} packets each")
    by_name = {t.name: t for t in txs}
    schedule = [by_name[n] for n in build_schedule([t.name for t in txs], args.n)]

    sent = {}                      # seq -> transmitter name
    counter = itertools.count(1)
    for i, tx in enumerate(schedule, 1):
        seq = next(counter)
        if tx.send(seq, args.extra):
            sent[seq] = tx.name
        print(f"\r{i}/{len(schedule)} packets, {len(sent)} away ", end="", flush=True)
        time.sleep(args.gap)
    print()

    # Long by default, and not only for packets still in the air. A receiver prints its
    # cumulative stage counters on its own schedule, so a line captured before the last packet
    # is a snapshot of the middle of the run - which is how one sweep came to report 19 packets
    # received and 14 in the counters that were supposed to explain them.
    print(f"waiting {args.settle}s for the last receptions and for the counters after them")
    time.sleep(args.settle)
    for r in rxs:
        r.close()

    for r in rxs:
        banner = [l for l in r.lines if "bridge up" in l]
        if banner:
            print(f"{r.name}: {banner[-1].split('bridge up: ', 1)[-1].strip()}")
        else:
            print(f"{r.name}: WARNING - never printed a boot banner, so nothing here says it "
                  f"was listening at all ({len(r.lines)} lines captured)")

    if not sent:
        print("\nnothing was transmitted: every packet was refused or unanswered, so there is "
              "no measurement here. Check that the transmitters are running this firmware and "
              "not sitting in the bootloader.")
        return 1

    # ---- results ----------------------------------------------------------
    table = defaultdict(lambda: defaultdict(int))
    totals = defaultdict(int)
    for seq, who in sent.items():
        totals[who] += 1
        for rx in rxs:
            if seq in rx.seen:
                table[who][rx.name] += 1

    width = max(len(t.name) for t in txs) + 2
    print()
    print("decoded, by transmitter and receiver")
    print(" " * width + "  ".join(f"{r.name:>12}" for r in rxs) + "     sent")
    for tx in txs:
        row = ""
        for rx in rxs:
            got, n = table[tx.name][rx.name], totals[tx.name]
            pct = 100.0 * got / n if n else 0.0
            row += f"{got:>5}/{n:<3} {pct:>3.0f}%"
        print(f"{tx.name:<{width}}{row}     {totals[tx.name]}")

    if args.out:
        args.out.write_text(json.dumps({
            "sent": sent,
            "seen": {r.name: sorted(r.seen) for r in rxs},
            "log": {r.name: r.lines for r in rxs},
        }, indent=1))
        print(f"\nraw result in {args.out}")


if __name__ == "__main__":
    sys.exit(main())
