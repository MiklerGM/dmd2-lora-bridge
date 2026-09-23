# Radio PHY notes (EBYTE E220-900 family)

What the module's own datasheet does and does not tell us about the air interface,
gathered while waiting for hardware. Source: EBYTE *E220-900T22S User Manual* (official,
cdebyte.com). The E220-900T30x DMD uses shares this PHY register model; only the power
table differs (30/27/24/21 dBm instead of 22/17/13/10).

## 2026-09-23: do two EBYTE receivers lose the same packets? Declared before running

Two findings leave a fork. The T-Deck and the E220 lose our packets independently of each
other, which says the dice are rolled per reception - but those are two different receivers
with different settings. And the E220 loses our packets before it registers a reception at all.
Two *EBYTE* receivers side by side separate the branches:

- If they lose the **same** packets, the outcome is fixed by the transmission: something in our
  signal varies packet to packet in a way EBYTE's receivers see and ours barely does. The search
  moves to what changes between our transmissions (oscillator start, PLL lock, SetTx timing).
- If they lose packets **independently**, our signal is steadily marginal for them and each
  reception is a fresh draw. The search moves to a constant difference of configuration or
  hardware.

**Bench, reverse direction, nothing moved between rooms.** A second E220 (E220-900T30, product
block `00 00 10 1E 0B 00 00`, on an ESP32 with a CP2102, `/dev/ttyUSB2`, `BENCH_OBSERVE` build)
lies 5 cm from the E22 with the antennas parallel; the Heltec is about 1 m away on the same
line. All three only receive. In the other room the T-Deck transmits our packets and the E220
there is the reference transmitter. Two receivers 5 cm apart are each other's neighbour - the
same arrangement as the T-Deck and E220 pair, noted rather than controlled.

Same-room placement is not a valid receiver position for a transmitter: the new E220, 1 m from
the E22 and the Heltec, read them at -7 to -12 dBm with either of two antennas. From the other
room the reference arrives at about -46 dBm.

**Arms.** `tdeck` - an ordinary packet; `e220` - the reference, sent immediately before every
T-Deck packet (`--precede e220`), so the new E220's register 1 holds a known reference value
before each of ours. 200 T-Deck packets, 200 reference packets, gap 0.8 s, seed recorded, the
new E220 queried after every packet. Fixed size; stopped early only for invalidity. Replicated
once under a second seed if valid; a verdict is claimed only if both runs give the same one.

**Decision**, `bench/analyze_pair.py`, over T-Deck packets, A = delivered at the E22, B =
delivered at the new E220:

- *underpowered* if either receiver delivered fewer than 8 - no verdict;
- *same packets* if P(B|A) >= 0.6 and P(A|B) >= 0.6;
- *near independent* if P(B|A) <= P(B) + 0.2 and P(A|B) <= P(A) + 0.2;
- *partial* otherwise, reported as a hypothesis for the next experiment.

A Mantel-Haenszel odds ratio over blocks of 25 T-Deck packets is reported with it: drift that
both receivers share would inflate the conditional shares, and a "same packets" verdict whose
block-stratified odds ratio falls below 3 is reported as possibly drift, not as a verdict.

**Invalid** if the reference reaches either EBYTE receiver below 90 %; any receiver lacks a boot
banner or a board drops out; the median reference level on the new E220's register 1 is above
-40 dBm (overload); fewer than 95 % of transmissions are acknowledged.

**Descriptive only:** the Heltec's view of the same packets (its stage counters and outcome
against each EBYTE receiver), and the new E220's register 1 and ambient-noise reading after each
packet. That last one is itself an exploratory finding from runs 1 and 2 below, not declared
beforehand: 0.8 s after a packet the E220's ambient-noise register reads -78/-79 dBm when the
module received the packet and -73 dBm when it did not, for our lost G and B packets and for S
alike - as if the module averages the channel over a couple of seconds and leaves out time it
spent receiving.

### Result of run 1: valid, underpowered, no verdict

`2026-09-23-pair-run1.json`, seed 20260926. The new E220 had its larger antenna back on, swapped
after the declaration was committed and before the first packet.

| T-Deck packets, n = 200 | E22 | new E220 | Heltec |
|---|---|---|---|
| delivered | 4 | 36 | 175 |
| reference delivered, n = 200 | 197 | 200 | 200 |

Valid by every declared criterion (reference at or above 98.5 %, all banners, every
transmission acknowledged, reference at -49 dBm on the new E220). But the E22 delivered only 4
of our packets against the 8 the rule needs, so by the declaration **there is no verdict**. None
of those four reached the new E220 as well (0.7 expected if independent). That leans towards
independence but is not a result: 0 of 4 does not exclude P(B|A) = 0.6.

The declared replication was not run. The same design would most likely be underpowered again:
the E22 took 2 % of the T-Deck's packets here and 10 % in the earlier reverse run. The better
pair for this question is two identical E220s with comparable rates, and that is a new
experiment with its own declaration.

Descriptive, not part of the decision:

- **A second E220 unit, and a transmitter without a front end, show the same stage:** register 1
  moved for none of the 164 T-Deck packets the new E220 lost (reference at -49 dBm, T-Deck at
  -60 dBm), and none of them produced AUX activity or UART output.
- The two EBYTE parts differ a lot on the same air: E22 2 %, E220 18 %.
- The Heltec again lost our packets only at the header (`hdr_err` 25 for 25 misses) and its
  outcome was independent of the new E220's (33 both, 31.5 expected) and of the E22's (2 both,
  3.5 expected).
- The ambient-noise difference did **not** appear: -83 dBm after lost T-Deck packets, -82 after
  delivered ones and after the reference. Here our packet arrived at -60 dBm rather than the
  Heltec's -39, and the averaging guess above predicted about 3 dB. Not reproduced; the guess
  about how the register averages should not be relied on.

## 2026-09-23: what the saved runs already say, and where inside the E220 our packets stop

### Re-reading the raw files (exploratory, no new packets)

- **Our own receiver loses our packets only at the header.** The T-Deck's final `hdr_err`
  equals the number of our packets it missed in every run that logged counters after the
  timestamp fix: 58 of 58 (`2026-09-23-standby-setfreq`), 21 of 21 (`-freq-sweep`), 4 of 4
  (`-native-fixed`). `crc_err` is 0 throughout, and E22 packets produced no header errors.
  The failure there is at sync/SFD/header, never at preamble detection or in the payload.
- **The two remote receivers fail independently on the same transmission.** Our packets that
  the E220 delivered but the T-Deck lost: 11 against 12.5 expected under independence
  (`standby-setfreq`, n = 200); pooled over seven runs, P(E220 | T-Deck ok) = 0.22 and
  P(E220 | T-Deck lost) = 0.13. A single per-transmission defect with two receiver thresholds
  would nest the losses; it does not. This does not show that the two receivers lose packets
  by the same mechanism.
- **Within the 200-packet run the E220 outcome looks like an independent draw.** Blocks of
  25: 2, 5, 7, 5, 6, 6, 6, 6; runs test z = +1.4 (no clustering). By predecessor, pooled over
  seven runs: after a delivered E22 packet 43/189 = 23 %, after our lost packet 83/424 = 20 %,
  after our delivered packet 19/108 = 18 %. T-Deck frequency error, RSSI and SNR of the same
  packet do not differ between E220-delivered and E220-lost packets.
- **The bridge filter dropped nothing but three-byte frames.** Across every saved log the only
  `rx dropped` lines are 3-byte frames heard by both remote receivers at once (29 s, 69 s,
  149 s after boot): the configuration read of the EBYTE *transmitter's* health probe leaking
  onto the air. No malformed or prefix-damaged payload reached either bridge.

### Declared before the first packet: the E220's "last received RSSI" register

With REG1 bit 5 set - which the stock template this bridge writes already sets - an E220
answers `C0 C1 C2 C3 00 02` in normal mode with register 0 (ambient noise) and register 1,
documented as the RSSI of the last received data. It is the only thing the module will say
about a reception it did not put on its UART. `-DBENCH_OBSERVE` (E220 bridge only) adds a
`RSSI` bench command for that read, logs every UART chunk as raw hex before any check and
every AUX low pulse with its start and width, and removes the 30 s health probe so the module
stays in normal mode after boot.

**Question.** For our packets the E220 does not output, does the module register a reception
(register 1 moves to our packet's level), and does anything reach its AUX or UART?

**Arms**, all on the Heltec at power level 3, each packet immediately preceded by an E22
packet (`--precede ref`) so register 1 holds a known E22 value before it; the three Heltec
arms are shuffled inside palindromic rounds, seed recorded; the E220 is queried 0.8 s after
every packet:

- `G` - an ordinary packet.
- `B` - `hdr=0000`: bytes 2-3 can never be valid (valid pairs XOR to A1). The PHY is identical
  to G, so the radio demodulates B exactly as often as G; the module's firmware rejects it
  after demodulation (0 of 90 such frames ever arrived). Positive control: does register 1
  move for a packet the radio received and the firmware refused?
- `S` - `sync=4444`: the radio cannot sync (0 % into an E22 in the sync sweep). Does register 1
  move on a preamble alone?
- `ref` - the E22, 150 packets, one before each Heltec packet.

**Size and stopping.** 50 packets per Heltec arm, 150 reference packets, fixed; stopped early
only for invalidity. Replicated once under a second seed if the first run is valid; a stage
conclusion is claimed only if both runs agree.

**Instrument check first, not used for conclusions:** a short sequence (about 10 E22, 10 G,
5 B, 5 S) to confirm the reply format, that register 1 follows delivered packets, that the
query does not go on the air (no 6-byte frame at the T-Deck), that the sync word reverts after
S, and how AUX behaves around a query. If delivered E22 and G levels are not at least 4 dB
apart there, all Heltec arms move to power level 2 and the check is repeated.

**Classification**, fixed now. A packet's E220 window runs from the previous query reply to
its own. Level L_E = median register 1 over delivered ref packets, L_H = median over
delivered G packets, both from the main run; a Heltec packet is *registered* when its reading
is on the L_H side of the midpoint. Only Heltec packets whose preceding ref packet was
delivered, and whose window holds no UART output other than their own, are classified.
u_G = registered share of undelivered G packets; u_B and u_S likewise over all B and S.

**Decision rules:**

1. *Invalid*, no stage conclusion: ref delivery < 90 %; |L_H - L_E| < 4 dB or fewer than 95 % of
   delivered G and ref packets classified to their own source; RSSI reply missing or malformed
   for more than 5 % of packets; any B or S packet delivered by the E220; the T-Deck receiving
   under half of G (sync not reverting); a board lost mid-run.
2. u_S >= 0.5: the register moves on a preamble alone, so "registered" only means "preamble
   detected"; report that and nothing finer.
3. u_S < 0.2 and u_B >= 0.1 (the register demonstrably moves for a packet the firmware refused):
   - u_G >= 0.5: most lost packets reach the stage that moves the register - the loss is after
     sync, in the payload or in the module's firmware.
   - u_G <= 0.2: most do not - the loss is in the radio before that stage, the same region
     where our own receiver fails.
   - otherwise mixed, reported as a hypothesis.
4. u_S < 0.2 and u_B < 0.1: the register appears to move only on output; the instrument cannot
   separate radio from firmware and no stage conclusion is drawn.

**Downstream**, also fixed now: for every undelivered G packet, any UART chunk or any AUX pulse
starting more than 400 ms before its window's query reply counts as the module having output
it. One such packet demonstrates a loss in the UART/bridge path; none in N bounds that path at
3/N (95 %). Every delivered packet should show one such pulse and one chunk, which validates
the two instruments. With the health probe absent, G delivery at or below 50 % against
ref >= 90 % shows the gap does not need the probe; a probe-on/probe-off comparison is not part
of this experiment.

### Result: our lost packets never reach the point where the module records a reception

`2026-09-23-register-run1.json` (seed 20260924) and `-run2.json` (seed 20260925), analysed by
`bench/analyze_register.py`, which implements the rules above:

| | run 1 | run 2 |
|---|---|---|
| reference delivered | 150/150 | 150/150 |
| G delivered | 6/50 | 6/50 |
| register moved, undelivered G (u_G) | 1/44 * | 0/44 |
| register moved, B (u_B) | 7/50 | 5/50 |
| register moved, S (u_S) | 0/50 | 0/50 |
| undelivered G with any AUX pulse or UART byte | 0/44 | 0/44 |
| delivered packets with one reception pulse and one chunk | 156/156 | 156/156 |
| L_E / L_H | -51 / -39 dBm | -49 / -39 dBm |
| delivered packets classified to their own source | 155/156 | 155/156 |

\* Seq 260 is not a movement: the reference before it read -44, an outlier on the Heltec side of
the -45 midpoint, and the G packet left that same -44 in place. The declared rule counts it.

Both runs are valid and both fall in the lower branch of rule 3, so by the declaration the stage
conclusion stands:

- **The E220 records a reception for a packet it demodulated and then refused, and not for one
  whose sync failed.** B moves register 1 as often as G is delivered - 12/100 against 12/100,
  which is what an identical PHY predicts - and every delivered packet moves it.
- **None of our 88 undelivered G packets moved it, and none produced AUX activity or a UART
  byte.** The loss happens before the module records a reception: in the radio's acquisition or
  decoding (preamble, sync, header or payload CRC), or in a firmware step that runs before that
  record and does not depend on bytes 2-3.
- **Excluded as the main path**, each by measurement in these runs: the module's frame check
  (B shows it comes after the record), the UART and the bridge (0 of 88 losses with any output,
  at most 3.4 % of losses at 95 %), and the 30 s health probe (absent in both runs, 12 % against
  100 % all the same).

What this does not establish: which radio stage. The event that moves register 1 is known only
to lie between sync and a completed read; if the firmware records it only after a CRC-good
RxDone, a payload CRC failure looks the same here as a header failure. Only the E220 was
observed, not the E22. No mechanism is identified.

Also measured in these runs:

- **Levels contradict the earlier table.** Register 1 puts the Heltec at -38/-39 dBm and the E22
  at -49 to -52 dBm at the E220: our transmitter is 10-12 dB *louder*. The table further down
  ("Heltec -50, E22 -44", the maximum of register 0 polled every 250 ms) has the order reversed.
  Different register, different day; not reconciled. -39 dBm is close to the -40 dBm this file
  uses as an overload warning, but the valid power sweep saw the same loss at -81 to -62 dBm.
- The T-Deck again lost our packets only at the header: `hdr_err` 17 for 17 misses and 12 for 12.
  Its outcome and the E220's radio-level outcome were again independent: 10 packets reached both
  against 10.8 expected (run 1, G and B).
- The E22 *transmitter's* health probe leaked `C1 00 08` onto the air three times in each run;
  all six landed in reference windows, so no Heltec packet was excluded.
- The instrument check (`2026-09-23-register-check.json`, 4 packets per arm, not used for
  conclusions) showed B moving the register 4/4 while G moved it 0/4, with the E22 at -46 rather
  than -50. The full runs did not reproduce that B rate (12/100 against 4/4). Unexplained; the
  5 dB shift in the E22's level says the bench changed between the check and run 1.

Next, not yet run:

- The same register on the E22 as target (REG1 bit 5 is clear there, so enabling it is a flash
  write) - whether an SX1262-based EBYTE receiver loses our packets at the same point.
- Our own receiver fails our packets at the header and never fails the E22's. An SX1262 receive
  setting that reproduced the EBYTE rate would give an observable model of the loss; nothing
  tried so far has (the symbol timeout made it selective against the E22 instead).
- An implicit-header transmission would test sync against header inside the module only if the
  firmware records RSSI on a header error. Semtech's reference driver passes RSSI only with
  RxDone, so that is expected to be uninformative and was not run.

## 2026-09-22: a frequency sweep, declared before it was run

Frequency has never actually been swept. What exists is an instantaneous-RSSI check, which at
BW500 accepts a quarter-megahertz of error and therefore only says we are in the right channel,
and a one-hertz step. The narrowband capture then measured something that makes the gap worth
closing properly: **the module that reaches every receiver transmits about 8.4 kHz from where we
do.** That offset between us and the part that refuses us has been measured and never varied.

Theory says it should not matter - a LoRa receiver is specified to acquire within a quarter of
its bandwidth, which here is 125 kHz, and 8.4 kHz is 1.7 % of it. Against that, an acquisition
limit rather than a signal-to-noise one is the only mechanism proposed so far that fits the
whole profile at once: re-rolled per transmission, flat across the valid part of the power
sweep, indifferent to payload and cadence, invisible to a receiver that shares our nominal
frequency, and equally fatal to RadioLib on the same crystal.

`foff=<signed kHz>` transmits one packet off channel, bounded at a quarter of the bandwidth.
The sweep is nine offsets on the Heltec plus an E22 arm in the same rounds, with the schedule
order shuffled inside each palindrome from a recorded seed.

Declared before the first packet, so that a shape found afterwards cannot choose its own
threshold:

- A frequency effect is claimed only if some offset arm beats the `foff=0` arm by at least
  20 points within the run, the E22 reference stays at or above 90 %, and the whole thing
  reproduces under a second seed.
- Anything short of that is reported as flat, and closes frequency across the swept range
  rather than being quoted as a trend.

### Result: flat, and frequency is closed

`2026-09-22-freq-sweep.json`, seed 20260922, 24 packets per arm, E220 the delivery oracle and
the T-Deck listening alongside it:

| kHz off channel | -40 | -16 | -8 | 0 | +8 | +16 | +40 | E22 |
|---|---|---|---|---|---|---|---|---|
| E220 | 1/24 | 0/24 | 2/24 | 4/24 | 5/24 | 2/24 | 3/24 | **24/24** |
| T-Deck | 22/24 | 19/24 | 20/24 | 19/24 | 24/24 | 23/24 | 20/24 | 24/24 |

The best offset beats the centre by four points against a declared twenty, with no shape across
the sweep and the reference at the ceiling throughout. **Deliberately transmitting up to 40 kHz
off channel neither helps nor hurts**, which is what the datasheet says should happen and is now
measured rather than assumed. The 8.4 kHz between us and the module is not what the module
refuses, and frequency is closed across this range.

The T-Deck row shows the transmissions were receivable: it decoded 147 of the same 168.

**What this run does not establish is that the offsets reached the air.** The T-Deck decoding
them proves nothing either way - 40 kHz is a fraction of what a BW500 receiver acquires through,
so it would decode an arm whose offset was silently dropped exactly as well. Neither receiver
was built with `SX_FREQ_ERROR`, so no log here measures the carrier. What is established is that
the code path ran: `air` rises from 331 ms to 427 ms whenever `foff` is present, which only the
standby and SetRfFrequency can account for, and the board echoes back the offset it parsed. The
remaining risk is confined to the frf arithmetic or the command itself being wrong, in which
case every arm was the same transmission and "flat" would be true and empty.

### Confirmed on the air, so frequency is closed

The T-Deck was rebuilt with `SX_FREQ_ERROR=1`, which logs RegFreqError per reception, and ten
packets were sent across the same offsets. What it measured:

| asked | -40 | -8 | 0 | +8 | +40 |
|---|---|---|---|---|---|
| RegFreqError | -40584 Hz | -8092 Hz | -36 Hz | +8147 Hz | +40635 Hz |

The offsets are real, the magnitudes are what was asked, and `foff=0` is the channel to within
36 Hz. The seven arms were seven different transmissions and all of them were delivered at the
same rate, so **frequency is closed across +/-40 kHz** rather than probably closed.

The zero arm is worth keeping for its own sake: our two SX1262 boards agree with each other to
within 36 Hz, while the EBYTE part sits about 8.4 kHz away from both. The carriers are not
drifting apart, and a sweep two hundred times that separation changed nothing.

### What the sweep did to the absolute rate, which is a separate question

Pooled over its seven arms this run delivered **17/168 = 10 %** to the E220, against a pooled
23 % before it. The arms are comparable to each other, which is what the criterion was about;
the block is not comparable to earlier runs, and one thing changed for all of it. Every packet
here is preceded by a standby and a SetRfFrequency about 100 ms before SetTx - the air figure
goes from 331 ms to 427 ms - because a centre arm that skipped what the offsets pay would have
differed from its own sweep by something other than frequency.

So this run cannot say whether that costs anything. It is worth its own within-run comparison:
`foff=0` against a line with no `foff` at all is exactly that difference and nothing else, and a
handle that reliably makes delivery worse is as useful as one that makes it better.


## 2026-09-22: standby and SetRfFrequency before SetTx, declared before it was run

The sweep left one difference between its block and every earlier run: each packet in it is
preceded by a standby and a SetRfFrequency, and `air` rises from 331 ms to 427 ms. Ninety-six
milliseconds is far more than two SPI commands cost, so the chip is in a different state when
SetTx arrives, and that state is the family of hypothesis this investigation keeps returning
to - what the oscillator and the synthesiser are doing as the first symbols go out.

`foff=0` against a line carrying no `foff` at all is that difference and nothing else. Both
arms run on the Heltec against an E22 arm in the same rounds, 100 packets each.

Declared before the first packet:

- A difference is claimed only at **15 points or more within the run**, with the E22 reference
  at or above 90 %, reproduced under a second seed.
- Fifteen rather than the usual twenty because the twenty-point rule exists for cross-run
  comparison at small n, where drift of sd ~0.10 swamps it. This is within one run against an
  in-run reference at 100 packets per arm, where the standard deviation of the difference at
  these rates is about five points. Below fifteen it is reported as no difference, and the
  10 % block stays an artefact of cross-run comparison rather than becoming a finding.
- A difference in either direction counts. A handle that reliably makes delivery worse is worth
  as much as one that makes it better, and this bench has not found one of either yet.

### Result: no difference, and the 10 % block was drift

`2026-09-23-standby-setfreq.json`, seed 20260923, 100 packets per arm:

| arm | E220 | T-Deck |
|---|---|---|
| `foff=0` - standby and SetRfFrequency before SetTx | 23/100 | 70/100 |
| no `foff` - straight into radio_send | 20/100 | 72/100 |
| E22 reference | **99/100** | 100/100 |

Three points, against a declared fifteen and a standard deviation of six. **Putting the chip in
standby and setting the frequency 100 ms before SetTx changes nothing**, and the ninety-six
milliseconds it costs buys and costs nothing on the air.

The more useful half is the denominator. Pooled over both arms the Heltec delivered 43/200 =
21.5 %, which is the pooled 23 % this bench has measured all along. **The 10 % in the frequency
sweep was drift**, nothing more, and the pre-declared threshold is the only reason it did not
become a fifth finding that later evaporated.

The control receiver makes the same point from the other side: the T-Deck took 88 % of the same
transmitter in the sweep run and 71 % here, moving down while the E220 moved up. Two numbers
from two runs an hour apart can differ by seventeen points in opposite directions on one link.
Nothing here is comparable across runs, and every number quoted from this bench needs its
reference inside the same run.


## 2026-09-22: two of the proposed controls, bounded from data already on disk

Both are still worth running; neither can be the explanation, and the bounds belong next to the
proposals so that an evening is not spent rediscovering them.

**The 30 s health probe.** `rx_task()` calls `e220_verify()`, which enters configuration mode,
flushes UART input and returns - an interruption of reception, and one captured escaping onto
the air. In `matrix-4x4.json` the two EBYTE logs span 108 s, so that probe fired about four
times per receiver during the run. Both EBYTE receivers took **24/24** from the other EBYTE
transmitter across it, and neither log contains a status transition. Zero losses in 24 paired
trials bounds the blanking at roughly 3.5 s per firing at 95 %, hence about three points of the
gap at the very most. The escaping configuration frame remains a real fault in the bridge and
should be fixed on its own merits; it is not what refuses us.

**The bare SX1262 next to the target.** In the same run the E220's neighbour is in continuous
RX while the Heltec transmits, and is not in RX at all while the T-Deck transmits - it is the
transmitter then. The E220 took 7/24 and 8/24 from the two. Removing a listening SX1262 from
beside the target changed nothing measurable, which bounds that contribution well below the
gap. It is not a clean isolation, because the transmitter changes with it, so a deliberate
sleep/RX alternation would still say more than this does.

## 2026-09-22: limits of the current diagnosis

The separated-room loss is reproducible. Its location inside the EBYTE receiver is still
unknown. Rechecking the saved `matrix-4x4.json` packet by packet gives 35 missing EBYTE
deliveries from our transmitters; **32 of those same packets were fully decoded by the
adjacent bare SX1262**. The final bare-receiver counters contain only three header errors
between them. Their counters cannot establish that EBYTE's missing packets died at its
header. EBYTE UART silence also includes module framing/filtering and the bridge's UART
receive path. The corrected XOR header remains necessary, but does not settle that boundary.

Several stronger statements below should therefore be read more narrowly:

- Repeating an identical payload disproves a deterministic payload blacklist; it does not
  establish independent per-transmission randomness, or place that randomness in the TX.
- The valid power sweep is evidence against a simple amplitude threshold. A comparison
  spanning 40 dB cannot use the near-pair cells already declared invalid through overload.
- An EBYTE control at 100% is valuable but is at the ceiling: it does not prove conditions
  were constant for the weaker DUT link. A null cadence result does not close every kind
  of receiver recovery or history dependence.
- The fixed four-node palindrome also assigns different predecessors to different trial
  positions. A history test needs balanced, randomized predecessors and recorded timing;
  comparing another pair of long runs would retain the drift problem.

There is an actual observation-path concern, independent of any RF hypothesis: every 30 s
`rx_task()` calls `e220_verify()`, which enters configuration mode, flushes UART input and
returns to normal mode. That is an interruption of reception, despite not writing NVM.
The earlier captured `C1 00 08` air frame shows that this path has misbehaved on hardware.
A future receive-only control should keep the module in normal mode after boot and record
raw UART bytes before the wire-payload filter, with AUX timestamps on its existing wire.
AUX activity alone is not proof that a LoRa packet was decoded.

### A new observation point without additional hardware

T-Deck can act as a narrowband power sampler: GFSK RX at 39 kHz bandwidth, reading instantaneous
RSSI every 100 us with actual timestamps saved. A chirp crossing the passband makes a pulse.
This observes timing before the LoRa demodulator, while the adjacent E220 still judges delivery.
It is not an IQ recording and cannot measure phase noise or certify the full transmitted waveform.
The instrument, qualification criteria and raw data are documented in
[`bench/results/README.md`](../bench/results/README.md#2026-09-22-narrowband-rssi-observation).

The central-frequency pilot passed the criteria fixed beforehand: six-pulse preamble trains
with fitted periods 4087--4102 us in all eight Heltec and all eight E22 captures, none in two
quiet windows. E220 received Heltec 2/8 and E22 8/8. Every window contains 10000 samples;
the maximum interval was 105 us, with no skipped slots.

After aligning the E22 trace by its additional twelve preamble symbols, thirteen dominant
pulses in the observed start-of-packet interval have corresponding peaks in every trace.
Two consecutive E22 peaks occur about 0.13 ms earlier; the other eleven median differences
are within 34 us. That pair's displacement is consistent in scale with the previously recorded
E22 frequency offset: opposite-slope chirps would differ by approximately
`2 * 8430 / (500000 / 0.004096) = 138 us`. This is a consistency check, not a fresh CFO
measurement or an explanation for lost packets. The central slice alone cannot identify chirp
direction. For Heltec's two accepted versus six rejected packets, no gross timing difference
is visible; that sample is too small to exclude a subtler effect.

The follow-up interleaves -100 and +100 kHz **receiver** offsets with both transmitters.
Both offsets pass the same qualification: Heltec 7/8 and 8/8, E22 8/8 at each, no quiet
chains. The failed Heltec qualification is an early unrelated burst before the packet,
which E220 received; the predetermined rule excludes it from alignment, not delivery counts.
E220 delivered Heltec 3/16 and E22 16/16 in this run.

The two suspected reverse chirps shift by 3.267--3.286 ms between the two slices, matching
the expected 3.2768 ms relative to the preamble grid. Both sources have that pair and the
same following strong timing landmarks; E22 retains its approximately 0.13 ms lead on the
pair. Seven selected landmarks are present in every one of the 47 qualified transmissions
across center and offset runs. Accepted and rejected Heltec packets do not separate on these
coarse timing features, but accepted counts remain small. The short final SFD portion can
fall outside these frequency slices, and RF phase is not observed: **this does not prove
the entire SFD or signal identical**. The exploratory overlay and its reproducible script
are in [`bench/results`](../bench/results/2026-09-22-waveform-review.png).

The next controls worth running on this bench, without moving boards, are:

1. Keep the target EBYTE in normal RX after boot, omit periodic configuration reads, and
   log raw UART before application filtering. Compare against the ordinary health-check
   path in short interleaved blocks with an EBYTE transmitter in every block.
2. Put the bare SX1262 next to that target alternately in RX and sleep. It is 10 cm from
   the target throughout every matrix trial; its electrical/RF contribution has not been
   isolated by the distant-TX power sweep. This tests a possible observer contribution,
   not a demonstrated interference source.
3. Balance and randomize the preceding transmitter/target's own TX history, with recorded
   timing jitter, while retaining paired EBYTE control packets. Longer fixed gaps alone
   do not test that interaction.

For a delivery-improvement claim in these proposed controls, require at least a 20-point
within-run difference reproduced with a second independent schedule seed, and assess
uncertainty by time blocks rather than treating every packet as an independent trial.
That is a proposed decision rule, not a significance claim for the present small captures.

## 2026-09-21 correction: bytes 2–3 are payload-dependent, not a counter

The counter interpretation introduced in `2a0f975` is wrong. With `c` equal to the XOR of
**all original payload bytes**, the E22 sends `A0 ^ c`, `01 ^ c` in bytes 2–3. The payload
itself is still XORed with `0x12`. These are full bytes, not four-bit fields: an eleven-byte
payload `00000000AAA` produces **E1 40**.

Measured on the current separated-room bench: 27 complete E22 frames, with repeated inputs,
the digit `1` moved through all eight positions, and payload lengths 8–100. Every capture
matches this formula and the complete expected encoded payload. Repeating an input repeats
the header; changing `00000005` to `00000015` changes the field from `A5 04` to `A4 05`,
although the transmit order increases. Those two observations distinguish it from a counter.

`firmware/src/ebyte_frame.h` now encodes this rule. The host regression uses complete captured
frames as golden values, including an odd length and repeated calls. This corrects a framing
defect; it does **not** by itself explain the remaining bare-SX1262 versus EBYTE delivery gap.

Consequently, an EBYTE UART silence is not proof of a PHY failure: the module can reject the
encapsulation after a successful LoRa decode. Past UART tests using a constant/copied header
with varying numbered payloads, or an independent counter, are confounded by that defect.
Sweeps using an even-length constant marker and `A0 01` are not invalidated by this finding.
The claim below that the layer above the PHY was already solved was premature.

The controlled comparison and full raw captures are retained in
[`bench/results`](../bench/results/README.md). Among 90 deliberately incorrect counter-header
frames, **none reached E220**; six accidentally correct ones in those same arms produced two
receptions. Correct headers yielded 19/48 with RadioLib and 8/48 with our TX, versus E22 48/48.
A subsequent comparison reversed the driver ordering (3/24 versus 10/24); do not treat the
first run's difference as proof of a persistent driver defect. Both still lose to E22.

In the normal ESP-IDF firmware, the corrected driver delivered **15/48** to E220 against
**46/48** from the interleaved E22; T-Deck received **44/48** and **48/48** respectively.
RadioLib's long interleaver at the same CR 4/5 was also tried: E220 counts were 8/24 versus
3/24 with normal interleaving, then **4/24 versus 4/24** on immediate repeat. E22 was 24/24
in both runs. This is not a confirmed improvement and is not enabled in the normal build.

Reversing the link without moving boards also reproduces the remaining loss: remote T-Deck
and E220 transmitted in alternating ABBA/BAAB blocks; the local **E22 received 5/48 from
T-Deck versus 46/48 from E220**. The local Heltec received 46/48 and 48/48, with all 94 numbered
frames matching the expected payload and corrected header. This confirms the gap on a distant
SX1262-based E22 receiver too, using a T-Deck without the Heltec front end. It does not locate
the loss inside the EBYTE radio versus its module firmware; that needs another observation
point, such as the module's SPI/IRQ traffic or an RF waveform capture.

The reverse run also captured an extra E220 frame containing `C1 00 08` after seq 51, followed
by a transient `radio stopped answering` on that module. Its register-read command reached
the air instead of producing a configuration reply. That frame is excluded from trial counts;
mode/AUX synchronization during the periodic health check remains an independent issue.

The `RX_SYMB_TIMEOUT` experiment also exposed an instrument failure: `IRQ_TIMEOUT` was ignored,
leaving the software's RX flag set after the radio stopped receiving. At a value of 8 the old
code received zero packets from **all three** transmitters, including E22. Rearming on timeout
restored reception and counted 2624 timeouts in 120 seconds. The knob also needs the sync-timeout
register update present in [Semtech's implementation](https://github.com/Lora-net/LoRaMac-node/blob/master/src/radio/sx126x/sx126x.c).
It did not reproduce E220's selectivity: even after that update it impaired E22 reception too.
It is left disabled in the normal build. Timing diagnostics now reset their timestamps after
failed receptions; formerly a later packet could inherit an earlier failed preamble's time.

## Confirmed from the datasheet — matches what DMD writes

| Register | DMD value | Datasheet meaning |
|---|---|---|
| REG0 air-rate bits 2-0 | `010` (AT+RATE=2) | air data rate **2.4k**, the module default |
| REG1 sub-packet bits 7-6 | `00` (AT+PACKET=0) | **200 bytes**, the default |
| Address | `0x0000` (AT+ADDR=0) | the manual exempts **only `0xFFFF`** from address filtering; `0x0000` is an ordinary address that happens to match, because every module on this bench uses it |
| REG3 transmission bit 6 | `0` (AT+TRANS=0) | transparent (broadcast), not fixed-point addressing |
| Channel | `0x12` = 18 | frequency = 850.125 + channel = **868.125 MHz** |

So an E220 set to `addr=0, channel=18, rate=2, packet=0, transparent` is a plain broadcast
listener/sender on 868.125 MHz — exactly the shape DMD's design relies on.

## Receive PHY identified, 2026-09-17 — measured on hardware

> Everything below this section was written while the PHY was unknown. It is kept because the
> reasoning that failed is worth reading, but **the inference it argues for is wrong** and is
> marked as such where it appears.

Air rate index 2 ("2.4k") on an E220-900T30D is:

| Parameter | Value |
|---|---|
| Frequency | 868.125 MHz (channel 18; 850.125 + CH) |
| Bandwidth | **500 kHz** |
| Spreading factor | **11** |
| Sync word | **0x55** |
| Header | explicit |
| CRC | on |
| IQ | normal |
| **Low Data Rate Optimize** | **on — and it must be forced** |

Coding rate is carried in the explicit header, so a receiver does not need it configured.

**Evidence:** an SX1262 set to exactly this decoded **18 of 20 beacon transmissions with the
payload CRC passing**, byte-identical every time, while all fifteen other sync words in the same
sweep decoded nothing. Flags read `PREAMBLE HDR_OK RX_DONE` with no `CRC_ERR`.

Two of these are not guessable and cost us most of a day:

- **Sync word 0x55** is neither the LoRa private 0x12 nor the public 0x34. No vendor or
  community source documents it; it was found by sweeping all 256 values *at the correct SF/BW*.
- **LDRO must be forced on.** Semtech's rule enables it above a 16.38 ms symbol, and SF11/BW500
  is 4.096 ms, so RadioLib's `autoLDRO()` leaves it **off**. EBYTE evidently follows the common
  "on for SF11 and SF12" shortcut instead. A mismatch leaves preamble, sync and header intact —
  the header block is always sent at reduced rate regardless — and corrupts only the payload, so
  the signature is `HDR_OK` plus a failing payload CRC. Call `forceLDRO(true)` after `begin()`.

### The trap: three configurations share one chirp slope

Chirp slope is `BW^2 / 2^SF`, and SF7/BW125, SF9/BW250 and SF11/BW500 all sit at 122.07 kHz/ms.
An SF11/BW500 chirp crossing a BW125 passband has the same slope as a genuine SF7/BW125 chirp,
so the correlator locks, re-arms and locks again roughly every ten symbols across a 226 ms burst.

Counting preamble detections per dwell therefore **rewards the aliases and punishes the answer**:

| Candidate | detections / 20 s | what it is |
|---|---|---|
| SF7/BW125 | 432 | alias, ~21 false locks per packet |
| SF9/BW250 | 123 | alias, re-arms ~3.5x slower |
| **SF11/BW500** | **71** | **the real one — one lock per packet** |

We read 432 as "found it" and filed 71 under noise. If a future sweep uses this metric, the
correct reading of a peak is "the signal's chirp slope is X", which names a *family* of three,
not a configuration. Bandwidth then has to come from somewhere else.

### What settled the bandwidth

FCC filings, which measure the thing directly rather than inferring it:

- [FCC ID 2ALPH-E220](https://fccid.io/2ALPH-E220/Test-Report/Test-report-DSC-4983213.pdf),
  EUT E220-400T30S: modulation LORA, **20 dB bandwidth 548.077 kHz**, at the factory default
  configuration — i.e. the same `REG0 = 0x62` we write. BW125 would read about 130 kHz.
- [FCC ID 2ALPH-E22900T22S](https://fccid.io/2ALPH-E22900T22S/Test-Report/Test-Report-DTS-4101474.pdf):
  6 dB EBW 576-592 kHz on the SX1262 sibling.

Slope 122.07 kHz/ms plus BW 500 kHz gives SF11 arithmetically. Independently, SF11/BW500 at
CR 4/5 is 2148 bps — the only one of the three that anyone would label "2.4k".

### The layer above the PHY (corrected 2026-09-21)

The PHY carries an EBYTE frame, not the payload directly. A 38-byte payload arrives as 46 bytes:

```
2C  CH  (A0 XOR c)  (01 XOR c)  00  00  00  LEN  |  payload XOR 0x12
c = XOR of all original payload bytes
```

| Byte | Meaning | How it is known |
|---|---|---|
| 0 | `0x2C` | constant across every capture |
| 1 | **channel number** | tracked 0x12 -> 0x13 when the beacon moved from channel 18 to 19 |
| 2-3 | `A0 XOR c`, `01 XOR c` | repeated/varied payload captures; the old constant markers all had `c = 0` |
| 4-6 | `00 00 00` | **unverified** - plausibly the address, but ours was only ever 0x0000 |
| 7 | **payload length** | 0x0A / 0x26 / 0x64 for payloads of 10 / 38 / 100 bytes |
| 8.. | payload **XOR 0x12** | see below |

Method: hold everything else still and vary one thing. Two payloads of the same length made of
different constant bytes separate framing from payload; two payloads of different lengths give
the overhead. `'A'` (0x41) arrives as 0x53 and `'B'` (0x42) as 0x50 — both exactly XOR 0x12, with
no dependence on position. Confirmed against the original base64 payload, which predicts
`"u[QSPS…` and produced exactly that.

**The obfuscation byte is a fixed 0x12, NOT the channel.** This is worth stating because the
coincidence is a trap: the first captures were all on channel 18, which is 0x12 in hex, so the
key and the channel byte were identical and the obvious reading was "the key is derived from the
channel". Moving the beacon to channel 19 moved the header byte to 0x13 and **left the XOR at
0x12**. Had we not checked, the rule written down here would have been wrong everywhere except
channel 18.

The backend prepends this header and XORs the payload on transmission. Receive currently checks
the marker and length, then strips the header and reverses the payload XOR; it does not enforce
bytes 2–3, so receiving our own incorrectly wrapped frames never validated their encapsulation.

**Still unverified:** what `0x2C`, `0xA0` and `0x01` mean, and whether bytes 4-6 really are the
address. Irrelevant for DMD interoperability, where the address is 0 as well, but it is an
assumption rather than a measurement.

### Corrections to our own register notes, from the manual

- `REG3` bit 4 is **LBT enable, and this build leaves it clear** — the request defaults to
  `lbt=0`, the built frame carries `REG3 = 0x00`, and the hardware reports `lbt=0` on every
  apply. An earlier version of this note claimed the opposite and reasoned from it that the
  beacon was not a clean 1 Hz. It is.
- `REG1 = 0x24` sets documented **reserved bit 2**; `0x20` is the clean value. Bit 5 (ambient
  RSSI register) is on and harmless.
- The -129 dBm sensitivity table below is **copied from Semtech** and describes the LLCC68's
  supported SF/BW envelope, not EBYTE's configuration. EBYTE's own headline figure for 2.4k is
  -146 dBm, which is impossible for any LoRa configuration. It is not evidence of anything.

---

## The residual unknown the datasheet does NOT close

EBYTE deliberately abstracts the LoRa PHY behind an "air data rate" index. The datasheet:

- does **not** bind an air-rate index to a specific SF/BW/coding-rate;
- gives only a sensitivity table of combinations the chip *can* use:
  `BW125/SF7 -124`, `BW125/SF9 -129`, `BW250/SF7 -121`, `BW250/SF10 -129`,
  `BW500/SF7 -117`, `BW500/SF11 -127` dBm;
- states **no sync word** and **no preamble length** anywhere.

**Best inference (NOT datasheet-confirmed) — and WRONG, see the SOLVED section above:** the 2.4k
default lines up with the module's typical -129 dBm sensitivity, which is either **SF9/BW125**
or SF10/BW250. The widely reported EBYTE default is SF9/BW125, so that is the leading guess —
but it is a guess, and the sync word and preamble are baked into the module firmware (and the
DMD/topicon build may have changed them).

> Measured answer: **SF11/BW500, sync 0x55, LDRO on**. The sensitivity table this argument rests
> on is Semtech's, reproduced by EBYTE, and says nothing about which configuration the module
> actually uses. The reasoning is kept as a record of how a plausible inference from a real
> datasheet number sent the search to the wrong end of the range.

### Tried, 2026-09-16: the inference did not hold, and 72 combinations are ruled out

A configured E220 transmitting real DMD traffic was swept against a Heltec V3.2 (SX1262) probe.
A **second E220 sat beside the probe as a control**, so "the transmitter went quiet" and "the
probe heard nothing" are separable — which mattered, because the first attempt was void: the
phone slept, and only 3 packets were on the air in 190 s.

| Sweep | Candidates | Control | Probe |
|---|---|---|---|
| SF9/BW125 + SF10/BW250, sync 0x12/0x34 | 4 | 132 packets, 12–14 in every 20 s window | **0** |
| Every LLCC68 SF/BW × sync 0x12/0x34 × IQ normal/inverted | **72** | 421 packets, 40–44 in every 65 s slice | **0** |

Zero packets, zero CRC mismatches and — the informative part — **zero header errors**. A header
error needs preamble and sync to correlate first, so its absence says the receiver never reached
sync lock at all. That exonerates the header mode (explicit vs implicit): a header-mode mismatch
would have locked and *then* failed. It leaves SF, BW, sync word and IQ polarity, all of which
the 72-candidate sweep covered.

Two alternative explanations were excluded experimentally rather than by argument:

- **Receiver overload.** At two metres the probe read −0.0 and −37 dBm instantaneous RSSI —
  saturation, which decodes nothing whatever the PHY. Moving the transmitter behind a glass door
  brought the worst reading to −61 dBm, well inside the SX1262's linear range. The nulls did not
  change.
- **Wrong frequency.** Instantaneous RSSI is measured after the channel filter, and it rose
  during transmissions while tuned to 868.125 MHz — so the energy really is in that passband.
  BW500 candidates additionally cover a ±250 kHz error.

**The one thing still missing is a positive control.** This probe has never decoded a LoRa
packet from anything. FEM on, a clean `begin()` and a drifting noise floor prove the radio is
powered and listening, not that the demodulator reports what it hears — so a bug in the receive
path is still formally indistinguishable from 72 honest misses. A `-DCONTROL_SWEEP=1` build
hunts ambient Meshtastic (sync 0x2B) and LoRaWAN (868.1/868.3/868.5) for exactly this reason;
run under open sky in a coastal town it found nothing in 52 candidates, which is as easily
explained by there being no traffic as by a broken decoder.

**Treat the 72 as bounded, not concluded, until something is decoded.** The cheapest way to get
there is a second Heltec: one transmits on parameters we choose, the other receives. The E22
would do the same and also reveal the SX1262-family rate mapping, but needs its own register
driver. After that, the remaining suspect is a vendor-private sync word — 256 values, which is
where an SDR stops being the fallback and becomes the right instrument.

## Consequences for the build

1. **Use a genuine EBYTE E220.** Because the exact SF/BW/sync are not derivable from public
   docs, a raw SX1262/LLCC68 board (Heltec/LilyGO/Meshtastic hardware) cannot be configured to
   match by reading a datasheet — it would need the PHY measured off the air first. The E220
   matches by construction; that is now the only documentation-supported path, not just a
   preference.
2. **Use the raw SX1262 board as the first PHY probe.** Repeated, known E220 packets let us
   scan the small set of plausible SF/BW/sync combinations and distinguish preamble, sync,
   header and CRC failures. An SDR remains the fallback if no candidate reaches a valid
   header.
3. The unknown closes empirically the moment a matching E220 receives a NOR7E packet (M5 → M6).

## Hardware decision — ordered for the interop bench

Ordered:

- **2 × EBYTE E220** — the reference path for M5 and the first NOR7E interoperability attempt.
- **1 × EBYTE E22** — ordered as a negative control, on the assumption that it must not receive
  the E220 link merely because both products contain an SX126x radio. **The assumption was
  wrong**: on air-rate index 2 it hears an E220 and an E220 hears it. It has been far more
  useful as a second, independent receiver, and as the SX1262 that proves the chip can do this.
- **1 × Heltec WiFi LoRa 32 V4-compatible ESP32-S3/SX1262 full kit** — a raw SPI radio
  test platform, supplied with an OLED and a separate GNSS module.
- **A separate set of 868 MHz antennas** — so every radio in the bench and field tests uses an
  antenna intended for the target band.

The raw SX1262 board is experimental, not a replacement for the E220 path and not yet a
supported firmware target. First complete E220 ↔ E220, then E220 ↔ NOR7E. After that, use the
raw SX1262 receive-only against repeated known E220 packets; attempt transmission only after
reception works. The OLED may later expose field diagnostics. The GNSS module does not change
the current ownership model: DMD2/the phone remains the source of rider position.

## Raw SX1262 — the settings that work

Measured, not inferred. See the SOLVED section at the top for the evidence.

```
frequency = 868.125 MHz      (channel 18)
SF        = 11
BW        = 500 kHz
sync word = 0x55             (NOT 0x12 and NOT 0x34)
header    = explicit,  CRC = on,  IQ = normal
LDRO      = forced ON        radio.forceLDRO(true) after begin()
```

Note that **coding rate does not have to match**: in explicit-header mode the CR is carried in
the packet header and the receiver recovers it, so a 4/5 vs 4/7 difference blocks nothing in
either direction. Only frequency, SF, BW, sync word and LDRO have to line up.

`forceLDRO(true)` is not optional and is the single easiest thing to leave out: RadioLib's
`autoLDRO()` computes it *off* for this configuration, which decodes the header cleanly and then
corrupts every payload byte. A build that omits it looks tantalisingly close and never works.

> The paragraph this replaced recommended SF9/BW125 because it matched a -129 dBm sensitivity
> figure and was also RadioLib's default. Both halves were true and the conclusion was still
> wrong; the sweep that disproved it, and the chirp-slope aliasing that made SF7/BW125 look like
> a strong hit, are written up at the top.

**How to recognise a candidate:** a DMD payload is ASCII base64 and its decoded header starts
with magic `0xD2`, version 2 or 3, a TTL and a packet type. The current version therefore starts
with **`0g`**, but that prefix alone is only a search hint: for example, `0gAA…` has the same
prefix and decodes to unsupported version 0. The probe decodes and checks the complete 12-byte
header before reporting a DMD candidate. A confirmed packet still requires the expected group
hash and a valid protocol CRC after de-obfuscation with that group ID. Bytes before the candidate
inside the same printable run would be evidence of module framing.

This is not Meshtastic-compatible: Meshtastic uses sync word 0x2B and its own presets and frame
format. The hardware can be reused, the firmware cannot.

## Why the probe is a separate program, not a radio backend

`probe/` is its own PlatformIO project with its own `main`. It is tempting to treat the SX1262
as "the bridge with a different transport", but UART versus SPI is the shallow half of the
difference. The deep half is that an E220 **hides** the PHY behind an index (`rate=2`) while an
SX1262 **exposes** it, and everything above says those parameters are still a guess. A backend
written now would bake that guess in as configuration, and when it failed there would be no way
to tell a wrong driver from a wrong guess.

The coupling also runs past the driver. `e220_config_t` is EBYTE vocabulary, and it is not
private: it goes to the phone in the `ST`/`CFG` contract (`ch`, `rate`, `packet`, `trans`,
`lbt`, `addr`), which the Android app parses. Generalising the radio means renegotiating that
contract too — so doing it before the SX1262's real requirements are known means guessing
twice.

The probe's job is also not the bridge's shape: walk a small grid, classify failures, dump what
arrives. It shares nothing with forwarding packets. Once the PHY is measured, a backend becomes
writable against facts, and `e220_write` / `e220_read_packet` are already the transport-neutral
seam it would attach to.

## The bench itself was the bug, 2026-09-17

Two boards half a metre apart do not measure a radio link. They measure a receiver in
saturation, and they answer every question with "no".

An SX1262 backend was declared broken on that bench: correct time on air to the microsecond,
a byte-perfect transmit buffer, `TxDone` on every attempt, `GetDeviceErrors` clean, every
command byte-identical to the RadioLib build sitting next to it — and 0 receptions out of more
than 300. A whole day went into bisecting a transmit path that worked.

**What gave it away** is an instrument worth keeping: the E220 will report the channel's noise
floor on demand. Set REG1 bit 5, then `C0 C1 C2 C3 <addr> <len>` returns register 0, the current
ambient RSSI, as `dBm = -(256 - value)` — and it answers in normal mode, without decoding
anything. Build the receiver with `-DE220_AMBIENT_RSSI=<poll interval ms>`.

It said the channel went from −90 dBm to **−16 dBm** during each transmission. The signal was
not missing; it was roughly fifty decibels too loud. The reference transmitter measured the
same, which is why *it* only managed 3 receptions in 22 and why that was read all day as "the
bench is marginal" instead of "the bench is broken".

Moved apart, the same firmware decodes.

**Two rules follow.**

- Every negative result from a saturated bench is void, including the ones that looked like
  careful bisection. "Still zero" was the receiver's answer to everything, so nothing tested
  that way was actually tested.
- Before trusting any link measurement, read the received level. A working link at these
  ranges sits near −70 dBm with the floor around −90. Anything above about −40 dBm means the
  receiver is being shouted at and its verdict is worthless.

The instrument costs packets if it is run too eagerly: each poll occupies the UART, and the
first version also flushed the input buffer, which drove reception to zero and briefly looked
like a second bug. Poll about once a second, and never flush.

## The whole matrix, in one run

Four boards, each transmitting 24 packets in its turn while the other three listen, all
interleaved so every pairing meets the same air in the same minutes. Raw result in
`bench/results/matrix-4x4.json`.

```
             listening:   heltec        e22         tdeck        e220
heltec transmits             -       3/24  12%†   22/24  92%    7/24  29%
e22    transmits         24/24 100%      -        24/24 100%   24/24 100%
tdeck  transmits         23/24  96%   6/24  25%       -         8/24  33%†
e220   transmits         24/24 100%  24/24 100%   24/24 100%      -
```

† Those two pairs sit 10 cm apart; the receiver is overloaded, not strict, and the cell means
nothing. The same trap read the other way would say the Heltec is worse than the T-Deck.

Dropping them leaves the honest comparison, everything across a room:

| | our receiver | a module's receiver |
|---|---|---|
| our transmitter | 92 %, 96 % | **29 %, 25 %** |
| a module transmitting | 100 %, 100 % | 100 %, 100 % |

This settles three things at once, in one run rather than an afternoon of them.

**The front end is not it.** A T-Deck has none and reaches a module 25 % of the time; a Heltec
V4.3 has a KCT8103L and reaches one 29 % of the time.

**The chip is not it.** The E22 is the same SX1262 we drive, behind EBYTE's own firmware, and
everything decodes it.

**The receiving chip family is not it.** An LLCC68 in the E220 and an SX1262 in the E22 refuse
us equally.

Modules hear everything and are heard by everything. The one failing region of the matrix is
our transmitter against a module's receiver, and it fails the same way for both of our boards
against both parts.

### Pooled, and two more classes closed

Across every across-the-room cell measured in one evening:

| | decoded |
|---|---|
| our transmitter into a module | **37/160 = 23 % ± 6.5** |
| a module into anything | **320/320 = 100 %** |

Single runs of 24 swing by fifteen points, so read that pooled figure and not the cells. Two of
the cells even swapped which of our two boards looked better between consecutive runs, which is
exactly the trap that has cost this project three findings.

**Cadence does not matter.** Quadrupling the gap between packets - 3.2 s instead of 0.8 - moved
our boards from 21 % and 12 % to 25 % and 25 %, both inside that swing, while the modules held
100 % in both, so the conditions were steady and the run counts. That closes recovery time in
the module's receiver, thermal accumulation, its UART being busy with the previous packet, and
our own receive cycle between transmissions.

**Holding the oscillator up does not matter.** STDBY_XOSC as the fallback mode plus 30 ms of
settle before SetTx, on one board while the other kept STDBY_RC: 21 % against 12 %, and the
sign between the two boards was the other way round in the run before. This was worth trying
because the failure is random per packet, indifferent to level across forty decibels and
indifferent to the payload - the shape of something re-rolled at each transmission, which a
TCXO that is powered down between packets and given 5 ms to restart certainly is. It is not
that.

The same shape is what makes the remaining candidates narrow. Whatever it is, it is decided
afresh for each transmission, it does not care how loud the packet arrives or what is in it,
and it is invisible to a receiver running our own configuration.

### A fourth finding that evaporated

Two candidates were tried with the right shape - both act at the very start of a transmission,
where this link loses its headers and never fails a CRC, and both are re-run on every packet.

Locking the synthesiser first, with SetFs and 20 ms of settle before SetTx: 7 of 32, 22 %.
Nothing.

Stretching the PA ramp from 200 us to 3.4 ms: **12 of 32, 38 %** in the first run, against 22.5 %
for that same cell pooled over every earlier run. Fifteen points, about one chance in fifty of
being noise. It was not reported as a finding, because the threshold had been set at 40 %
beforehand and three findings had already evaporated here.

It evaporated too. With the slow ramp on both boards: 10 of 40 and 7 of 40. Pooled over all
three runs, 29 of 112 - 26 % against a baseline of 23 %, which is nothing.

Worth writing down as a method note rather than only a result. A single run of 32 produced a
fifteen-point jump at p = 0.02 that does not exist, and had it been believed, the next day
would have gone into refining a ramp time.

### Level, measured at the module that refuses us

Read the maximum, not the median: the instrument polls every 250 ms against a 320 ms packet, so
half the samples land in the silence between packets.

| what the far E220 sees | peak level | decoded |
|---|---|---|
| silence | -75 dBm | - |
| our Heltec, across the room | -50 dBm | 29 % |
| an E22, across the room | -44 dBm | **100 %** |
| our T-Deck, 10 cm away | -10 dBm | 33 % |

**Six decibels apart, and one decodes always while the other decodes a quarter of the time.**
At -50 dBm against a -78 floor there are twenty-eight decibels of margin, so being quieter is
not the explanation and can be retired with a number rather than an argument.

The last row is why only the across-the-room cells should be quoted. -10 dBm is at the input
limit of these parts, so that cell is compromised too - in the other direction from the Heltec's
near cell, and by an amount nobody has measured.

## What a transmitter's own timing says about it

The receiver timestamps the three stages instead of only counting them, so a transmitter's
coding rate and preamble length can be read off the air rather than asked for. Header-valid to
rx-done is the payload and nothing else - at SF11/BW500 a 46-byte frame is 258 ms at CR 4/5,
303 at 4/6, 348 at 4/7, 393 at 4/8 - and preamble-detected to header-valid measures the
preamble. Measured on one receiver, both transmitters interleaved:

| transmitter | preamble to header | header to done |
|---|---|---|
| this driver | 80 ms | **220 ms** |
| an E22 module | 120 ms | **220 ms** |

**The coding rates are the same.** That closes a hypothesis worth naming, because it was
arrived at honestly: a module's packets do arrive about 210 ms later in an interleaved schedule
than ours, which looks like a longer packet, and CR 4/8 would have accounted for 135 ms of it.
It is not that. Most of the 210 ms is the module's own UART and handling latency. Transmitting
at CR 4/8 was tried anyway and changed nothing at an E220 - 1 of 24, against 23 of 24 for the
module beside it.

The preamble genuinely differs: 40 ms is about ten symbols, so a module sends roughly twice our
eight. It is not the cause either. Preambles of 8, 12 and 20 all decode at an E220 about one
time in sixteen, and RadioLib reaches the same receiver five times more often while sending the
same eight symbols we do.

This also withdraws an earlier note here claiming the module's preamble is *shorter* than ours.
That came from the ambient-RSSI instrument, which polls about once a second and cannot see a
300 ms burst at all.

## RadioLib on the same board reaches an E220; this driver does not

Measured 2026-09-21, one Heltec V4.3, one bench, the two builds minutes apart.

| transmitter on that board | E220 decoded | our own SX1262 receiver |
|---|---|---|
| the probe, driven by RadioLib | ~22 in 200 s, about one in five | 63 |
| this driver | **0 of 30** | 28 of 30 |

Both use the same front end and the same two receivers, one in another room.
**Correction:** this did not isolate the register sequence or use identical frames: the
RadioLib probe used a constant marker while the driver's numbered payload needed different
header bytes. The payload-dependent fields at the top of this document must be held correct
before attributing a difference to either driver's SPI or PHY behavior.

The same day ruled out a regression in our own history: the firmware from `3e10b41`, the commit
that recorded 22.0 % over 2211 trials, was rebuilt and reflashed. It sent about 80 packets in 240
seconds, our receiver took 70 of them and the E220 took none. Nothing changed here since is
responsible.

### Two measurements from that day that do not mean what they look like

**A receiver ten centimetres away proves nothing.** An E22 at that range decoded nothing from
this driver - and nothing from RadioLib either, which decodes fine at room distance. It is
overloaded, not strict. Any conclusion of the form "it fails even in contact, so the failure is
categorical" is void.

**The modem's SNR estimate saturates.** At -58 dBm this driver reads 7.50 dB and an E22 reads
7.38 dB, which says only that both are above the estimator's ceiling. It cannot compare two
transmitters until the signal is near the sensitivity limit.

### What the errata registers actually hold

Read on hardware rather than argued about, at three moments:

```
untouched        0889=04  08D8=C8  0736=0D  08E7=18
after configure  0889=00  08D8=3E  0736=0D  08E7=38
before SetTx     0889=0A  08D8=3E  0736=0D  08E7=38
```

A read-modify-write from those reset values gives exactly the constants this driver writes, and
bit 2 of 0x0889 is still clear at transmit time, as erratum 15.1 requires at 500 kHz. Errata
15.1, 15.2 and 15.4 are all correctly applied here. LDRO was tested too, which a link between
two of our own boards could never do because both ends are forced into the same non-standard
mode: with it off our own receiver loses us, as it must, and an EBYTE still decodes nothing.

## SX1262 transmit into an E220: measured, still unexplained

An SX1262 receives an E220 perfectly and transmits to one badly. Both directions carry the same
PHY and the same frame; only the direction changes.

| link | share decoded |
|---|---|
| E220 → SX1262 | ~100 % |
| an EBYTE module → an EBYTE module | **100 %** |
| SX1262 → EBYTE, this driver | **22.0 %** (95 % CI 20.3-23.8) |

Those are the numbers after excluding slots where our own transmission collided with the control
beacon, which is the correction that matters most here: **the control was also the interferer.**
Reconstructing every transmit slot from the reception timestamps (the fit accounts for 536 of 536
receptions within ±200 ms) splits the run cleanly:

| | trials | decoded |
|---|---|---|
| collision-free (\|offset\| > 0.45 s) | 2211 | **22.0 %** |
| colliding | 948 | 5.2 % |

So the 17 % this document used to quote is a mixture of a 22 % link and a 5 % collision floor with
30 % of the weight on the floor. The same correction applied to the reference transmitter takes it
from 96.9 % to **939 of 939 — exactly 100 %**, its 65 losses being 59 collisions with us, 6 inside
one 24-second blackout, and none unexplained.

**The true comparison is 100 % against 22 %.**

That also proves the receiver hears us perfectly well: our transmissions *deny the channel* to a
link that is otherwise flawless, and the losses concentrate exactly where the control starts
inside our packet. Whatever is wrong, it is not that nothing arrives.

**Two independent drivers fail identically** - but the evidence for that is the byte-for-byte diff
of their SPI command streams, not their rates. At this bench's run-to-run swing (below) 26 % and
22 % are indistinguishable.

### Ruled out by measurement, not by argument

Each was a sweep of four variants interleaved at 2 s and labelled by payload letter, so every
variant met the same conditions and drift cancelled.

- **Transmit power.** Flat, and this time with one variable. The first sweep moved `paDutyCycle`
  and `hpMax` along with the level at every step, and its collapse at the bottom - 12/156 against
  117/491, p < 1e-5 - therefore belonged to either. Repeated with the pair pinned at Semtech's
  validated (4, 7) and only `SetTxParams` moving, over 24 dB with no beacon on the air:

  | chip output | decoded, n ≈ 88 each |
  |---|---|
  | +22 dBm | 22.6 % |
  | +14 dBm | 27.1 % |
  | +6 dBm | 24.9 % |
  | −2 dBm | 20.4 % |

  A 4.5-point spread over 16 dB; top three against the bottom, z = 0.87. **The collapse did not
  reproduce**, so it was the off-table PA pair rather than the level. Note also that the earlier
  "flat across ~40 dB of received level" was wrong: within one run the measured span is 19 dB,
  −81 to −62 dBm, and the 40 dB came from combining runs of different geometry.
- **The front end.** Powering the KCT8103L down and leaning on the chip's own output made things
  worse, not better (2.8 % and 20.7 % against 31.7 % with it on). Its receive path is proven good
  by the ~100 % reverse direction. **This does not exonerate its transmit path**: powering the
  part down does not remove it from between the radio and the antenna, and the documented modes
  do not describe an unpowered through-path, so that experiment compares "amplifier on" against
  "something undefined" rather than against "amplifier absent".
- **PA duty-cycle/hpMax pair.** Semtech's validated (4,7) at 22 dBm is no better than the
  off-table (2,1) at 2 dBm, p = 0.46.
- **TCXO settling.** Holding STDBY_XOSC for 10/40/100 ms before transmit does not help. The 5 ms
  startup is real and measurable - it shows as exactly 5 ms more time on air - but it is not this.
- **Preamble length.** 8/16/32/64 symbols: 20.6 / 23.7 / 20.5 / 23.3 % on collision-free slots,
  n ≈ 550 each, χ² = 2.78 on 3 df. A well-powered null, and the treatments are *proven* to have
  reached the air: the reception timestamps carry them, at +29/+93/+232 ms above the shortest
  preamble against +33/+98/+229 ms predicted from the symbol count. The E220's own preamble was measured indirectly
  through the RSSI instrument's view of its on-air span: it is *shorter* than ours, not longer.
- **A varying field in the EBYTE header.** Bytes 2-6 are constant in the E220's own transmissions
  too, so duplicate suppression would punish it equally.

### A wrong register, but not a fix

`SetPaConfig` resets the over-current limit, and both drivers rewrite it to `0x18` - the SX1261's
60 mA, where the SX1262's high-power PA is specified at `0x38`. Wrong in RadioLib too, and worth
correcting on the part number alone.

**It changes nothing measurable, and the first measurement saying otherwise was noise.** One run
put it at +11 points, p = 0.01; the same design run again put it at -7. Together, 36/288 against
45/288, p = 0.28.

Worth keeping as a caution: both runs interleaved their four variants every 2 s, so drift between
runs is not the explanation. At ~72 attempts per cell and a ~15 % success rate, this link swings
further than a binomial model predicts - fading and interference move on timescales far longer
than the interleave. **Do not call an effect of this size real on one run, however clean the
design looks.**

### The chip is not the problem

An **E22-900T22D** - EBYTE's own module, an SX1262 behind their MCU - was put on the bench beside
the Heltec and both transmitted to the same E220 receiver in one run:

| transmitter | decoded |
|---|---|
| E22, an SX1262 configured by EBYTE | **153/200, 76.5 %** |
| this driver, same PHY, same frame | 5-9/72, ~10 % |

+66.8 points, z = 12.3, p < 0.0001. A bare SX1262 talks to an E220 perfectly well. Whatever is
wrong is in this driver's configuration or in the Heltec's transmit chain, and it is not an
incompatibility between the two chip families.

The E22 also settles a question about the bench itself: it decodes the E220 at 64 % and this
driver at 25 %, the same lopsidedness the E220 receiver shows. Two receivers of different chip
families agree, so the defect is in what is transmitted, not in one receiver's fussiness.

### The sync word: a hole in our own foundations, now closed

A reviewer asked a sharp question: did the 256-value sweep that found "sync 0x55" establish what
an E220 *transmits*, or only a setting on which we *receive* it? Those are not the same claim,
and a permissive receiver would hide the difference.

He was right that we had not checked. RadioLib's eight-bit argument expands to registers
`(high | 0x04, low << 4 | 0x04)`, so sweeping all 256 arguments visited 256 of 65536 possible
register pairs - every combination of the two **high** nibbles, with both low nibbles pinned at
4. This driver copied that same pair, `0x54 0x54`, from the same formula.

**Measured, receive side** - the sweep rerun over register pairs directly, 30 s per candidate
against an E220 beacon, three cycles:

| pair | decoded |
|---|---|
| `5454`, `5555`, `5050`, `5455`, `5554` | 15, 15, 15, 15, 15 - identical |
| `5F5F` | 0 |
| `4444`, `6464` | 0 |

So the high nibbles are matched strictly and the low ones only partly: 0, 4 and 5 are
interchangeable, F is not. Our pair sits inside a tolerant family, which is exactly why receive
could never answer the question.

**Measured, transmit side** - four pairs interleaved every 2 s for 776 s, ~94 transmissions each,
into an E22 receiver:

| pair | decoded |
|---|---|
| `5454` (what we send today) | 27.8 % |
| `5555` | 34.2 % |
| `5050` | 28.8 % |
| `4444` (control) | **0 %** |

`5454` against the other two working pairs: +3.7 points, p = 0.52. **No effect.** The control's
clean zero proves the registers really were being rewritten per packet, so this is a negative
result rather than an instrument that did nothing.

No effect, then - though "no significant difference at this sample size" is what was measured,
which is weaker than proof of no effect, and the three pairs may have been fewer than three
conditions if the low nibbles are reserved as RadioLib's headers claim. What is solid is that the
high nibbles match and the control was silent. It was worth the day either way: the hypothesis
fitted every property of the defect -
transmit-only, power-independent, per-packet, identical in two drivers that share the formula,
absent on a module that writes its own registers. It simply is not true.

*An early look at this run showed 27 % against 49 % and 44 % on five and nine receptions. It was
noise, and it vanished by ~90 per cell. Third time on this bench: the same happened with the
over-current limit and with the antenna.*

### Why effects keep evaporating, quantified

The link's own success probability wanders. Over 200 s blocks the collision-free rate runs from
4.5 % to 45.6 %; across 32 blocks χ² = 100.4 on 31 df, z = +8.8, implying the underlying p drifts
with **sd ≈ 0.09-0.11** and a correlation time of about **115 s**.

The operational consequence is the single most useful number on this bench: **a 600 s run holds
only ~5 independent draws of that drift, so its mean carries ±4.5 points of non-binomial error on
top of the binomial.** Two runs cannot be compared for anything below roughly **20 points**.

Interleaving variants every 2 s is fully common-mode against this and is unaffected - the design
was right. Only cross-run comparison is poisoned.

That does **not** account for all three effects that evaporated, and an earlier version of this
paragraph claimed it did. Taken one at a time:

- **The antenna** was a genuine cross-run comparison, 17.6 % against 11.9 % in two separate runs.
  The drift explains it completely.
- **The sync word** never left one run: it went from 27 % against 49 % on five and nine
  receptions to nothing at ~90 per cell. Plain small-sample noise, no drift needed.
- **The over-current limit** is the awkward one. Both estimates were within-run and therefore
  common-mode against drift, and yet they came out +11 and -7. That eighteen-point swing is
  further than binomial noise on ~144 per arm comfortably reaches. The combined result is a
  clean null, so the conclusion stands - but *why* the two runs disagreed that much is not
  explained, and it is a reason to keep replicating rather than a solved question.

### The product-information block

Registers `0x80-0x86` are read-only "product information, 7 bytes" - documented for the E22,
undocumented for the E220, answered by both. Two samples:

```
E220, 30 dBm:  00 00 10 1E 0B 00 00
E22,  22 dBm:  00 22 10 16 0B 00 00
                  ^^ ^^ ^^
                  |  |  +-- rated power in dBm: 0x1E = 30, 0x16 = 22
                  |  +----- same on both 900 MHz parts; plausibly the band
                  +-------- differs by family
```

The power byte is what the firmware now reads at boot, which retired the per-variant build flags.
The band byte is the one worth pinning down next, because it is the only field that would catch a
mistake nothing else can see: a 400 MHz part accepts channel 18, reads its registers back
correctly, and tunes to 428 MHz instead of 868. That needs a sample from another band.

### What is missing to go further

An E220 reports one bit per packet: decoded or not. Every hypothesis left is about analogue signal
quality, and one bit cannot separate them. The instrument that can is the probe's view - preamble
detections, header-valid, CRC errors, RSSI and SNR per attempt - and it needs **a second bare
SX1262**, because the only one here is the transmitter under test.
