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
import random
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
stop = threading.Event()

def to_port():
    # readline, not iteration: iterating a pipe reads ahead and holds a line back until the
    # buffer fills, so a command sent now would leave minutes later, or never.
    try:
        while True:
            line = sys.stdin.buffer.readline()
            if not line:
                break
            port.write(line)
            port.flush()
    finally:
        stop.set()

threading.Thread(target=to_port, daemon=True).start()
try:
    while not stop.is_set():
        # Forward available chunks: pyserial.readline reads byte by byte and can stall
        # a large diagnostic record long enough to exhaust the board's USB TX buffer.
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
finally:
    port.close()
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

TX_REPLY = re.compile(r"\bTX(OK|FAIL) seq=(\d+)\b")
# The bridge logs "rx 38 bytes: 00000042AAAA..." - the payload starts with its sequence number.
RX = re.compile(r"rx \d+ bytes: (\d{8})")


def tx_result(line, seq):
    for match in TX_REPLY.finditer(line):
        if int(match[2]) == seq:
            return match[1] == "OK"
    if "TXERR" in line:
        return False
    return None


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
        self.seen, self.lines, self.times = {}, [], []
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
                self.times.append(time.time())
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
                result = tx_result(line, seq)
                if result is False:
                    print(f"  ! {self.name} refused seq={seq}: {line.strip()}")
                if result is not None:
                    return result
            time.sleep(0.05)
        print(f"  ! {self.name} never answered for seq={seq}")
        return False

    def command(self, text, expect, timeout=3.0):
        """Send a console command; returns the first new line containing expect, or None."""
        with self.lock:
            mark = len(self.lines)
        self.write(text + "\n")
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                fresh = self.lines[mark:]
            for line in fresh:
                if expect in line:
                    return line
            time.sleep(0.02)
        print(f"  ! {self.name} did not answer {text}")
        return None

    def query(self):
        """Ask a BENCH_OBSERVE receiver for its RSSI registers; returns the reply line or None."""
        return self.command("RSSI", "RSSIQ")

    def close(self):
        self.stop.set()
        if self.proc:
            # EOF asks the remote process to close its serial port before ssh exits.
            try:
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
            try:
                self.proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=3.0)
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
        self.lines, self.times = [], []
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
            self.times.append(time.time())
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
            # A read may stop partway through seq=10; do not accept its prefix as seq=1.
            lines = buf.split("\n")
            buf = lines.pop()
            for line in lines:
                result = tx_result(line, seq)
                if result is False:
                    print(f"  ! {self.name} refused seq={seq}: {line.strip()}")
                if result is not None:
                    return result
        print(f"  ! {self.name} never answered for seq={seq}")
        return False


def build_schedule(names, n, rng=None):
    """The order to transmit in: ABBA rounds, exactly n packets per arm.

    With an rng the order within each round is shuffled before it is mirrored. The palindrome is
    what cancels linear drift and stays either way; the shuffle is what stops one arm always
    following the same predecessor. A fixed order guarantees that, and no gap length removes it:
    under ABBA our packets arrive at a receiver that has just decoded the reference packet, and
    the reference arrives at one that has just heard nothing.
    """
    out, quota = [], defaultdict(int)
    while any(quota[x] < n for x in names):
        half = list(names)
        if rng is not None:
            rng.shuffle(half)
        for name in half + half[::-1]:
            if quota[name] < n:
                out.append(name)
                quota[name] += 1
    return out


def self_check():
    """Check scheduling and acknowledgement matching without a board."""
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
    # Shuffling the order within a round must not cost the quota or the mirror, which are the
    # two properties the whole design rests on.
    for seed in (1, 2, 20260922):
        names = ["a", "b", "c", "d"]
        s = build_schedule(names, 6, random.Random(seed))
        assert len(s) == 24, (seed, len(s))
        for x in names:
            assert s.count(x) == 6, (seed, x, s.count(x))
        for i in range(0, 24, 8):
            assert s[i:i + 8] == s[i:i + 8][::-1], (seed, s[i:i + 8])
    print("schedule: all checks pass")

    # A late reply for another packet must neither confirm nor reject this transmission.
    from types import SimpleNamespace
    for chunks, expected in (
        ([b"TXOK seq=10\nTXFAIL seq=1\n"], False),
        ([b"TXFAIL seq=10\nTXOK seq=1\n"], True),
        ([b"TXOK seq=7 air=1\nTXFAIL seq=1\n"], False),
        ([b"TXOK seq=1", b"0\nTXFAIL seq=1\n"], False),
    ):
        node = SimpleNamespace(name="check", lock=threading.Lock(), lines=[])
        node.write = lambda _: node.lines.extend(b"".join(chunks).decode().splitlines())
        assert Node.send(node, 1, "", timeout=0.2) is expected, chunks
        replies = iter(chunks)
        port = SimpleNamespace(write=lambda _: None, flush=lambda: None,
                               read=lambda _: next(replies, b""))
        tx = SimpleNamespace(name="check", port=port)
        assert Transmitter.send(tx, 1, "", timeout=0.2) is expected, chunks
    print("acknowledgements: all checks pass")


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
    rng = random.Random(args.seed) if args.seed is not None else None
    schedule = [by_name[x] for x in build_schedule([n.name for n in nodes], args.n, rng)]
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
            "arms": {name: {"tx": tx.name, "extra": extra} for name, tx, extra in arms},
            "seed": args.seed,
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
    ap.add_argument("--extra", default="", help="extra TX tokens for every arm, "
                                                "e.g. 'power=0 len=64'")
    ap.add_argument("--arm", action="append", metavar="NAME=TX[:TOKENS]",
                    help="one schedule arm: a transmitter plus the TX tokens that define it, "
                         "e.g. 'low=heltec:foff=-40'. Repeatable, and several arms may share a "
                         "transmitter - that is how a setting is swept against an in-run "
                         "reference. Without any, each --tx is one arm.")
    ap.add_argument("--seed", type=int, help="shuffle the arm order within each palindromic "
                                             "round. Recorded in --out; without it the order is "
                                             "fixed, so every arm has the same predecessor "
                                             "every round.")
    ap.add_argument("--gap", type=float, default=0.4, help="seconds between packets")
    ap.add_argument("--precede", metavar="ARM",
                    help="send this arm before every packet of every other arm, so each of "
                         "those follows the same known predecessor; --n counts the others")
    ap.add_argument("--toggle", metavar="NAME",
                    help="a --rx receiver running BENCH_OBSERVE that sleeps (M0=M1=1) and wakes in "
                         "blocks of --block packets of the other arms, ABBA, first state from "
                         "--seed; used to switch a neighbour off")
    ap.add_argument("--block", type=int, default=10)
    ap.add_argument("--states", default="SLEEP,WAKE",
                    help="the two console commands --toggle alternates, e.g. 'TUNE 0,TUNE 8200'; "
                         "the board must answer '<command> ok'. The second is the resting "
                         "state, restored at the end")
    ap.add_argument("--probe", action="append", metavar="NAME",
                    help="a --rx receiver running BENCH_OBSERVE: after every packet and --gap, "
                         "ask it for its RSSI registers and record the reply")
    ap.add_argument("--settle", type=float, default=35.0,
                    help="seconds to wait at the end for the last packets and for a receiver "
                         "to print its cumulative counters after them")
    ap.add_argument("--out", type=Path, help="write the raw result here as JSON")
    ap.add_argument("--node", action="append", metavar="NAME=[HOST:]PORT",
                    help="a board that both transmits in its turn and listens the rest of the "
                         "time; give several and every pairing is measured in one run")
    ap.add_argument("--self-check", action="store_true",
                    help="check scheduling and acknowledgements; needs no hardware")
    args = ap.parse_args()
    if args.self_check:
        return self_check()
    if args.node:
        return matrix(args)
    if not args.tx or not args.rx:
        ap.error("at least one --tx and one --rx are required, or use --node")

    # A transmitter on another machine is driven through the same ssh node a matrix uses.
    txs = [(Node if ":" in s.split("=", 1)[1] else Transmitter)(*s.split("=", 1)) for s in args.tx]
    by_name = {t.name: t for t in txs}
    if args.arm:
        arms = []
        for spec in args.arm:
            name, _, rest = spec.partition("=")
            txname, _, tokens = rest.partition(":")
            if txname not in by_name:
                ap.error(f"--arm {name}: no --tx named {txname!r}")
            arms.append((name, by_name[txname], " ".join((args.extra, tokens.strip())).strip()))
    else:
        arms = [(t.name, t, args.extra) for t in txs]
    probed = set(args.probe or [])
    writable = probed | ({args.toggle} if args.toggle else set())
    rxs = [(Node if s.split("=", 1)[0] in writable else Receiver)(*s.split("=", 1)) for s in args.rx]
    for r in rxs:
        if isinstance(r, Receiver):
            r.start()
    probes = [r for r in rxs if r.name in probed]
    toggle = next((r for r in rxs if r.name == args.toggle), None)
    if len(probes) != len(probed) or (args.toggle and not toggle):
        ap.error("every --probe and --toggle must name a --rx")
    # Long enough for an ssh session to come up and for a board that reset when its port was
    # opened to finish booting. A receiver still booting is a receiver that misses packets.
    time.sleep(6.0)

    print(f"{len(arms)} arms on {len(txs)} transmitters, {len(rxs)} receivers, "
          f"{args.n} packets each, seed {args.seed}")
    for name, tx, extra in arms:
        print(f"  {name}: {tx.name} {extra or '(no extra tokens)'}")
    rng = random.Random(args.seed) if args.seed is not None else None
    # Drawn before the schedule so a seed fixes both. ABBA over blocks cancels a linear trend.
    first = (rng or random.Random()).random() < 0.5
    pair = [x.strip() for x in args.states.split(",")]
    if len(pair) != 2:
        ap.error("--states takes exactly two commands")
    states = pair if first else pair[::-1]
    arm_by_name = {a[0]: a for a in arms}
    names = [a[0] for a in arms]
    if args.precede:
        if args.precede not in arm_by_name:
            ap.error(f"--precede {args.precede}: no such arm")
        order = []
        for name in build_schedule([x for x in names if x != args.precede], args.n, rng):
            order += [args.precede, name]
    else:
        order = build_schedule(names, args.n, rng)
    schedule = [arm_by_name[n] for n in order]

    sent = {}                      # seq -> arm name
    tx_times, probe = {}, {}       # seq -> [sent, answered]; seq -> {receiver: reply}
    neighbour, state = {}, None    # seq -> the toggled receiver's state when it went out
    per_block = args.block * (2 if args.precede else 1)
    counter = itertools.count(1)
    for i, (name, tx, extra) in enumerate(schedule, 1):
        if toggle and (i - 1) % per_block == 0:
            b = (i - 1) // per_block
            want = states[[0, 1, 1, 0][b % 4]]
            reply = toggle.command(want, f"{want} ", timeout=8.0)
            if reply is None or "FAILED" in reply:
                print(f"\n  ! {toggle.name} did not confirm {want}; stopping")
                break
            state = want
            time.sleep(1.0)
        seq = next(counter)
        if toggle:
            neighbour[seq] = state
        t0 = time.time()
        if tx.send(seq, extra):
            sent[seq] = name
        tx_times[seq] = [t0, time.time()]
        print(f"\r{i}/{len(schedule)} packets, {len(sent)} away ", end="", flush=True)
        time.sleep(args.gap)
        if probes:
            probe[seq] = {r.name: r.query() for r in probes}
    print()

    # Long by default, and not only for packets still in the air. A receiver prints its
    # cumulative stage counters on its own schedule, so a line captured before the last packet
    # is a snapshot of the middle of the run - which is how one sweep came to report 19 packets
    # received and 14 in the counters that were supposed to explain them.
    print(f"waiting {args.settle}s for the last receptions and for the counters after them")
    time.sleep(args.settle)
    if toggle and state != pair[1]:
        toggle.command(pair[1], f"{pair[1]} ", timeout=8.0)   # leave it in the resting state
    for r in rxs + [t for t in txs if isinstance(t, Node)]:
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

    width = max(len(a[0]) for a in arms) + 2
    print()
    print("decoded, by arm and receiver")
    print(" " * width + "  ".join(f"{r.name:>12}" for r in rxs) + "     sent")
    for name, _tx, _extra in arms:
        row = ""
        for rx in rxs:
            got, n = table[name][rx.name], totals[name]
            pct = 100.0 * got / n if n else 0.0
            row += f"{got:>5}/{n:<3} {pct:>3.0f}%"
        print(f"{name:<{width}}{row}     {totals[name]}")

    if args.out:
        args.out.write_text(json.dumps({
            "arms": {name: {"tx": tx.name, "extra": extra} for name, tx, extra in arms},
            "seed": args.seed,
            "sent": sent,
            "seen": {r.name: sorted(r.seen) for r in rxs},
            "log": {r.name: r.lines for r in rxs},
            "log_t": {r.name: r.times for r in rxs},
            "tx_log": {t.name: t.lines for t in txs if isinstance(t, Node)},
            "tx_times": tx_times,
            "probe": probe,
            "neighbour": neighbour,
            "argv": sys.argv,
        }, indent=1))
        print(f"\nraw result in {args.out}")


if __name__ == "__main__":
    sys.exit(main())
