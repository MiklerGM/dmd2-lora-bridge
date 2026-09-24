# Radio PHY notes (EBYTE E220-900 family)

What the module's own datasheet does and does not tell us about the air interface,
gathered while waiting for hardware. Source: EBYTE *E220-900T22S User Manual* (official,
cdebyte.com). The E220-900T30x DMD uses shares this PHY register model; only the power
table differs (30/27/24/21 dBm instead of 22/17/13/10).

## 2026-09-24: our receiver told to expect a module's preamble, declared before running

SetPacketParams carries the preamble length for reception as well as for transmission. Every
preamble test so far changed only what our transmitter sends (8 to 128 symbols, no effect at an
E220); our receivers were always configured for 8. The modules transmit 20 (E22) and 30 (E220)
and, presumably, receive expecting the same - which cannot be changed on them, but can be copied
onto our SX1262. If our receiver, told to expect 20 or 30, starts refusing our packets while still
taking the modules', the loss would be reproduced on a receiver we can instrument. (It would not
mean our preamble is too short: 32-128 symbols were already sent to an E220 without effect.)

**Bench.** The Heltec (8-symbol preamble), the E22 (20) and the E220 (30) transmit from here; the
T-Deck receives, running a bench image with a new `RXPRE <n>` console command
(`08257b12790d50541a610024f7f6f97e662fa95fd074d0e93f5029cccd0714e6`), and the antenna-less E220
records delivery descriptively.

**Run.** `bench/interleave.py`, arms `heltec`, `e22`, `e220`, 300 packets each, palindromic rounds
shuffled by seed 20261018, gap 0.8 s; the T-Deck cycles `RXPRE 20`, `RXPRE 30`, `RXPRE 8` in
palindromic blocks of 15 packets (`--toggle tdeck --states 'RXPRE 20,RXPRE 30,RXPRE 8' --block 15`).

**Decision**, `bench/analyze_rxpre.py`, T-Deck delivery of the Heltec's packets under each setting
against `RXPRE 8`, bootstrap over switching blocks, 99 % interval:

- *reproduces the modules' selectivity* if at least 30 points lower, interval below 0, with the E22
  and the E220 still at 90 % or more under that setting; repeated under seed 20261019 before it is
  claimed;
- *no effect detected* if within 15 points; *partial* otherwise.

**Invalid** if a switch is not confirmed, a board drops out, or under `RXPRE 8` the T-Deck takes the
E22 or the E220 below 90 %.

## 2026-09-24: Heltec, E22 and E220 on one scope, declared before capturing

The T-Deck/E220 comparison left two readings open: the post-jump lead belongs to the E22 (the
odd one), or to the Heltec (the lagging one). One instrument, three transmitters, settles which.
The E220 with its antenna was moved beside the E22 (laptop `/dev/ttyUSB2`); the Heltec, the E22
and it transmit from here. The T-Deck in the far room is the scope again (it received all three
without saturating in a 4-packet check); the antenna-less second E220 there records delivery.

**Capture.** `bench/rssi_capture.py`, arms `heltec`, `e22`, `e220`, slices -107, -100, 0, +93, +100,
+108 kHz, 12 captures per arm per slice plus quiet windows, seed 20261017.

**Decision**, `bench/analyze_three.py`, counting only slices where both arms of a pair qualify
(chains in at least 75 % of their captures), 99.5 % intervals, the E220's preamble fitted as before:

- **valid** only if the positive control holds - E22 at least 30 us ahead of the Heltec, interval
  clear, at two or more slices - and the E220's preamble length is unambiguous;
- **Q1**, E220 minus Heltec: *the E220 leads like the E22* if at least 30 us earlier, interval clear,
  at two or more slices; *no lead detected* if no slice differs by 30 us either way; *mixed* otherwise;
- **Q2**, E22 minus E220: *the EBYTE modules differ* if at least 30 us apart, interval clear, at two or
  more slices; *no difference detected* otherwise.

Reading, fixed now: Q1 lead with Q2 no difference means the two EBYTE modules share a path the
Heltec lags - and with the T-Deck matching an E220 earlier, the lag would be the Heltec's own, not
what the T-Deck's losses need. Q1 no lead with Q2 difference means the E22 is the odd one. Either
way the post-jump lead would not be what separates our transmitters from EBYTE's.

### Result: the E22 is the odd one; the post-jump lead is closed as a cause

`2026-09-24-rssi-three.json.gz` (228 captures, seed 20261017; raw file outside git), result
`2026-09-24-rssi-three.result.json`. Valid: every arm qualifies at every slice, the E220's preamble
is again unambiguously 30 symbols, and the positive control holds. The antenna-less E220 took the
Heltec 3/72, the E22 71/72 and the E220 72/72 meanwhile.

| near-jump slice | -107 | -100 | +93 | +100 | +108 kHz |
|---|---|---|---|---|---|
| E22 minus Heltec | **-71.6** | **-38.5** | -47.3 | **-58.5** | **-31.3** |
| E220 minus Heltec | -6.8 | +5.6 | -46.5 | -14.5 | -14.4 |
| E22 minus E220 | **-64.9** | **-44.1** | -0.8 | -44.1 | -16.8 |

(bold: interval clear of 0.) **Q1 no lead detected; Q2 the EBYTE modules differ.** By the reading
fixed beforehand, the E22 is the odd one: the E220 - received by everyone, always - follows the
same post-jump path as our Heltec, and the E22 runs ahead of both. With the T-Deck matching an
E220 earlier, the lead that four captures measured is real but belongs to the E22, not to the
difference between our transmitters and EBYTE's. **It is closed as an explanation.**

Exploratory, not declared: comparing the E220 and the Heltec over every landmark at all six slices
(preamble end, both sync symbols, the SFD, the whole header), everything agrees within about
+/-20-30 us; whole-header shift -2 to +4 us, SFD shift near zero as the 164 Hz carrier offset
predicts, one SFD landmark at -21 us just clear of zero. **At this instrument's resolution - some
tens of microseconds in time, a few kilohertz in the frequency path - the always-received E220
and our mostly-lost Heltec transmit the same start of packet.** Whatever separates them is finer
than that, or in phase, or not in the shape of the chirps at all.

## 2026-09-24: our board without a front end against an E220, declared before capturing

Everything about the post-jump lag so far compares one pair: the Heltec (with its KCT8103L) and
the E22 (SX1262 under EBYTE's firmware). Two questions that pair cannot answer: does our board
**without** a front end lag too, and does an EBYTE module of the **other** chip family, the
LLCC68 in an E220, behave like the E22?

**Bench.** The scope moves to the Heltec (`probe` environment `heltec_rssi_scope`: the same
sampler, the front end held in receive through its LNA). In the far room the T-Deck (bench image,
antenna on) and the first E220 (antenna on, `/dev/ttyUSB1`) transmit, 10 cm apart; the second E220
has no antenna and does not transmit. The local E22 records delivery.

**Capture.** `bench/rssi_capture.py`, arms `tdeck` and `e220`, slices -107, -100, +93, +100, +108 kHz
(near the openings of h6 and h7) and 0 kHz (far from any jump), 12 captures per arm per slice plus
quiet windows, seed 20261016. Each arm and slice must qualify by the 2026-09-22 criteria.

**Decision**, `bench/analyze_family.py`. The E220's preamble length is fitted as the one that makes
the most header landmarks common to both arms, and must beat the runner-up by a factor of two
(otherwise invalid). Then E220 minus T-Deck at the five near-jump slices, 99.5 % intervals:
*the EBYTE module leads after a jump* if at least 30 us earlier, interval clear of 0, at two or
more slices; *no lead detected* if no slice differs by 30 us with its interval clear; *mixed*
otherwise. A lead would put the lag in our SX1262 as we configure it, on either board, against
EBYTE's firmware on either chip; no lead would put it in the Heltec or in the E22.

### Result: no lead detected, on thin ground - and what that does to the post-jump lead

`2026-09-24-rssi-family.json.gz` (156 captures, seed 20261016; raw file outside git), qualification
`2026-09-24-rssi-family.analysis.json`, result `2026-09-24-rssi-family.result.json`. Heltec scope
image `f02ffb1540954349d895b6c6879e7046d74ec664473c94c2e8ebf41b2b727193`; the T-Deck back on
`v0.1.0-84-gf4dd5c6`. The E22 took the T-Deck 17/72 and the E220 71/72 meanwhile.

- **The E220's preamble is 30 symbols**: that length makes 47 header landmarks common to both arms,
  no other length more than 8. (Ours is 8, the E22's 20.)
- **Qualification**: the E220 qualifies at -100 and +100 kHz only (preamble chains in 8, 12, 8, 2,
  10, 5 of 12 captures at -107, -100, 0, +93, +100, +108 kHz); the T-Deck everywhere. The E220, at
  21 dBm and received through the Heltec's LNA, most likely saturated the scope - the same failure
  as the strong PA arms. By the declaration only -100 and +100 kHz count.
- **E220 minus T-Deck**: -4.8 us (-67.3 to 58.6) at -100 kHz, 123 us after h6's jump; +39.7 us
  (-61.7 to 60.0) at +100 kHz, 121 us after h7's. **No lead detected.** For comparison, the E22 led
  the Heltec by 34-48 and 60-69 us at these same two points in every T-Deck-scope run.

The intervals are wide - about +/-60 us, against +/-20-40 on the T-Deck scope - and only two
slices qualify, so this is not proof that the T-Deck and the E220 follow the same path after a
jump. But both point estimates sit near zero or on the wrong side, not at -40 to -70. If it holds,
it takes the post-jump lead out of the explanation: the T-Deck loses its packets to EBYTE
receivers as badly as the Heltec does, and an EBYTE module that is always received (the E220)
would then follow the same post-jump path as the T-Deck. The difference seen between the Heltec
and the E22 would belong to one of those two boards - the Heltec's front end, or the E22 - not to
"our transmitter against EBYTE's". To settle it, the comparison has to be repeated on one
instrument with levels that do not saturate it: the Heltec, the E22 and an E220 on the same scope.

## 2026-09-24: does the PA's drive level move our post-jump path? Declared before capturing

A synthesiser's frequency right after a jump can be pulled by what the power amplifier does to the
supply and to its own load; on the Heltec that includes the front end. The `power=` token cannot
test it cleanly (it re-applies the configuration and leaves it in force), so a new token `pa=0..3`
sets, for one packet, SetPaConfig at Semtech's (4, 7) pair and SetTxParams at +22/+14/+6/-2 dBm - the
clean power sweep's settings - and restores the configured level afterwards.

**Arms** on the Heltec: `p22` (`pa=0`), `p14` (`pa=1`), `p6` (`pa=2`), `m2` (`pa=3`), and `e22`. Same
five slices as the settings screen (-107, -100, +93, +100, +108 kHz), 12 captures per arm per slice
plus quiet windows, seed 20261015. The strongest arm may saturate the scope's RSSI in the far room;
an arm or slice that fails the qualification is excluded, not rescued.

**Decision**, `bench/analyze_knobs.py --control m2 --arms p6 p14 p22`: valid only if `e22` leads
`m2` by at least 30 us, interval clear, at two or more slices; an arm moves **toward the E22** if at
least 30 us earlier than `m2`, interval clear, at two or more slices; **away** for the mirror image;
otherwise **no change detected**. Twenty-four decibels of drive with no change would put the PA and
the front end's loading out of the post-jump difference.

### Result: no change detected where it could be measured; the strong arms saturated the scope

`2026-09-24-rssi-pa.json.gz` (310 captures, seed 20261015; raw file outside git), qualification
`2026-09-24-rssi-pa.analysis.json`, result `2026-09-24-rssi-pa.result.txt`. Heltec image
`v0.1.0-98-g409b893` (`f1268568f003e3163580677ac13d95fc30c0c68ded3dc7409e0955360eb9497c`).

The positive control holds: `e22` leads `m2` (-2 dBm) by 66.5, 34.0, 60.4 and 35.8 us at -107,
-100, +100 and +108 kHz, intervals clear (at +93 kHz -78.0 with its interval reaching +19.4). But the
stronger arms overloaded the scope in the far room: preamble chains in 0-4 of 12 captures at most
slices, so by the declaration those arm-slices are excluded. What qualifies:

| arm minus `m2` | +93 kHz | +100 kHz | +108 kHz |
|---|---|---|---|
| `p6` (+6 dBm) | +26.2 (-13.0 to 180.3) | **+31.9** (5.2 to 83.0) | +10.1 (-15.5 to 52.1) |
| `p14` (+14 dBm) | excluded | +16.1 (-10.8 to 70.6) | excluded |
| `p22` (+22 dBm) | excluded | +33.1 (-8.1 to 77.5) | excluded |

(-107 and -100 kHz: every arm above `m2` excluded.) **No change detected** for any arm by the
declared rule. What little qualifies points, if anywhere, the other way - more drive a little
*later*, further from the E22 - and only one slice clears zero. The PA's drive does not bring our
path toward the E22's over 8 dB (-2 to +6 dBm) at three slices; above that the test is one slice
wide and says little. Delivery at the first E220 in the same run, descriptive: `p22` 14/60, `p14`
13/60, `p6` 8/60, `m2` 6/60, `e22` 58/60.

## 2026-09-24: does any transmit setting move our post-jump path toward the E22's? Declared before capturing

The post-jump lag is a property of the signal itself, measurable in minutes and without waiting
for a delivery rate to settle. So settings are screened by it first; only one that moves our path
toward the E22's goes on to a delivery test.

**Arms**, all on the Heltec except the reference: `ctl` (plain), `txm` (`txmod=1`: bit 2 of 0x0889,
which Semtech's erratum 15.1 ties to modulation quality at 500 kHz), `ldo` (`ldo=1`), `cal`
(`cal=1`: RC, PLL and ADC recalibrated at 868.125 MHz right before SetTx), and `e22`. Transmit
power is left out: the `power=` token re-applies the whole configuration and leaves it in force
for the following packets, which would contaminate the other arms.

**Capture.** Scope slices -107 and -100 kHz (h6, 65 and 123 us after its jump) and +93, +100 and
+108 kHz (h7, 64, 121 and 187 us), where the jump capture found the E22 leading by 36-87 us.
`bench/rssi_capture.py` with the arms above, 12 captures per arm per slice plus two quiet windows
each, seed 20261014. Each arm and slice must qualify by the 2026-09-22 criteria.

**Decision**, `bench/analyze_knobs.py`, landmark difference arm minus `ctl` at those five slices,
99.5 % bootstrap intervals:

- **valid** only if the positive control shows: `e22` minus `ctl` at least 30 us earlier, interval
  clear of 0, at two or more slices;
- an arm moves **toward the E22** if it is at least 30 us earlier than `ctl`, interval clear, at
  two or more slices (and not later at two); **away** for the mirror image; otherwise **no change
  detected**.

An arm judged *toward the E22* is then tested for delivery under its own declaration.

### Result: valid, and none of the three moves our path

`2026-09-24-rssi-knobs.json.gz` (310 captures, seed 20261014; raw file outside git), qualification
`2026-09-24-rssi-knobs.analysis.json` (every arm and slice qualifies), result
`2026-09-24-rssi-knobs.result.txt`. A first attempt aborted at packet 12: the Heltec still ran the
2026-09-22 image, which did not know the new tokens. It was reflashed with `v0.1.0-95-gda80f63`
(same flags, front end powered, same register readback) and the capture rerun under the same seed;
the aborted file is kept outside git and not used.

Positive control, `e22` minus `ctl`: -79.7, -47.3, -56.1, -68.9, -40.2 us at the five slices, every
interval clear of 0 - the post-jump lead seen for the fourth time. Arms minus `ctl`:

| arm | -107 | -100 | +93 | +100 | +108 kHz | verdict |
|---|---|---|---|---|---|---|
| `txm` (0x0889 bit 2) | -2.8 | +1.5 | -0.1 | -2.6 | +5.1 | no change detected |
| `ldo` | -2.0 | +3.3 | +32.5 (-68.9 to 101.7) | -8.0 | -0.3 | no change detected |
| `cal` | +0.4 | -1.1 | +3.2 | -0.6 | -11.8 | no change detected |

The erratum bit, the regulator and a recalibration at the working frequency all leave our path
where it was, 40-80 us behind the E22's right after a jump. Delivery at the first E220 in the same
run, descriptive: `ctl` 12/60, `txm` 17/60, `ldo` 8/60, `cal` 12/60, `e22` 60/60.

## 2026-09-24: the first half-millisecond after a symbol's jump, declared before capturing

A direct test of the hypothesis below. Two header symbols with known values are followed: h6
(v = 553, starts at -115.0 kHz) and h7 (v = 1373, +85.2 kHz). A scope slice f above a symbol's start
sees its crossing (f - f0) / 500 kHz x 4096 us after the symbol's opening jump, so placing slices at
a series of distances samples our path and the E22's through the first 0.65 ms after the jump.

**Slices.** h6: -112, -107, -100, -92, -85, -78, -70, -55, -35 kHz; h7: +88, +93, +100, +108, +115,
+122, +130, +145, +165 kHz - about 25, 65, 120, 190, 245, 300, 370, 490 and 655 us after each jump.
`bench/rssi_capture.py`, all 18 offsets interleaved, 12 captures per transmitter per offset plus
two quiet windows each, seed 20261013, same bench and transmitters as the fine capture; the scope
image as before. Each offset qualifies by the 2026-09-22 criteria.

**Decision**, `bench/analyze_jump.py`, on each symbol's E22-minus-Heltec difference at its landmark
(17.98 for h6, 18.58 for h7), relative to that symbol's own baseline (median from 500 us on - the
fine capture showed a constant +20 us far from jumps, which is not the effect):

- *transient confirmed* if **both** symbols show a dip at least 30 us below the baseline 80-200 us
  after the jump and a peak at least 30 us above it 220-400 us after, each with its 99.5 % interval
  clear of the baseline, and settle within 20 us of it from 500 us on;
- *no near-jump difference* if neither symbol departs from its baseline by 30 us, interval clear,
  anywhere up to 400 us - which refutes the hypothesis at this resolution;
- *mixed* otherwise, reported with the whole curve.

### Result: mixed - the dip is real in both symbols, the overshoot is not established

`2026-09-24-rssi-jump.json.gz` (468 captures, seed 20261013), qualification in
`2026-09-24-rssi-jump.analysis.json`, curve in `2026-09-24-rssi-jump.curve.txt`. Seventeen of the
eighteen offsets qualify; +165 kHz does not (preamble chains in 0/12 E22 and 5/12 Heltec captures)
and is excluded, which removes h7's 655 us point. The first E220 took the Heltec 34/216 and the E22
213/216 meanwhile.

| after the jump | h6: E22 minus Heltec | h7: E22 minus Heltec |
|---|---|---|
| ~25 us | -61.6 (-89.5 to -11.4) | -47.5 (-93.5 to -2.7) |
| ~65 us | **-84.9** (-161.5 to -45.6) | **-87.2** (-147.7 to -17.7) |
| ~120 us | -35.7 (-93.7 to -12.8) | -69.2 (-117.5 to -28.4) |
| ~190 us | -21.9 (-73.6 to 33.0) | -57.5 (-81.2 to -35.6) |
| ~245 us | +35.2 (-5.6 to 66.1) | -24.6 (-56.9 to 9.4) |
| ~300 us | +50.1 (3.4 to 64.2) | +43.9 (-12.0 to 77.3) |
| ~370 us | +4.2 | -2.5 |
| ~490 us | +26.4 | +6.9 |
| ~655 us and h6's far slices (1.7-2.0 ms) | +9.5; -5 to +16 | excluded |

Baselines (median from 500 us on): h6 +8.1 us, h7 none (0). By the declared rule: **dip** in both
symbols (30 us or more below baseline, 80-200 us after the jump, interval clear) - yes; **peak**
(30 us above, 220-400 us, interval clear) - no for either (h6 +50 at 300 us has its interval
reaching 3.4, below the +8 baseline; h7 +44 reaches -12); **settled** by 500 us - yes for both.
Verdict **mixed**.

What stands: for the first roughly 200 us after each of these symbols' opening jump, the E22's
path crosses a given frequency 50-90 us before ours - at the chirp's 122 kHz/ms, ours is about
10 kHz (40 bins) behind the E22's in that interval - in both symbols, at several slices each, and
in agreement with the two earlier captures. After about half a millisecond the two paths agree.
A positive swing near 300 us appears in both symbols but does not meet the declared bar, so
"ringing" is not established; "our path lags the E22's right after a jump" is. The difference
cannot say which of the two departs from an ideal chirp.

## 2026-09-24: a model of the scope traces, and where ours departs from the E22's (exploratory)

After aligning on the preamble crossing at a slice f, an upchirp of value v that starts at time s
crosses f at s - v/2048 if f lies above its start frequency f0 = -250 + v x 500/2048 kHz, and one
symbol later if below (the chirp wraps first). With the header block starting at 12.25 symbols
(8 preamble, 2 sync, 2.25 SFD), **every landmark at all four offsets is explained by one header
symbol with one value**, consistently across slices - h0 348, h1 1147, h2 1659-1679, h3 819-840,
h4 1843-1864, h5 328, h6 553, h7 1352-1413 (the spread is the landmark resolution); the two sync
symbols come out as v = 40, nibble 5 times 8. The instrument and the alignment do what they
should.

`2026-09-24-boundary-model.py` assigns each landmark and sets the E22-minus-Heltec difference
against how soon after its symbol's opening frequency jump the crossing happens
(`2026-09-24-rssi-fine.boundary.json`, and for the 2026-09-22 captures
`2026-09-22-rssi-old.boundary.json`):

| crossing after the jump | E22 minus Heltec, new capture | same, 2026-09-22 captures |
|---|---|---|
| 123-164 us | -47.5 (h6 at -100 kHz), -63.7 (h7 at +100) | -56.4 (h6), -59.3 (h7) |
| 287 us | +63.5 (h2 at +190; interval 1.3 to 112.5) | - |
| 370-410 us | +22.8, +22.2 | +22.2 |
| 530 us and later | within about +/-20 us, no pattern | the same |

The two reproduced local differences are the two crossings closest to a symbol's opening jump,
and the difference changes sign within a few hundred microseconds and dies out by about half a
millisecond - the shape of a synthesiser's damped step response, period of order 300 us, rather
than of a time shift or a different symbol value. **Hypothesis, not a finding:** right after the
frequency jumps that every BW500 symbol contains (at its boundary and at its wrap), our
transmitted frequency path differs from the E22's for a few hundred microseconds - about a tenth
of each 4.1 ms symbol. That would not show in SNR or level, and could upset a receiver's fine
synchronisation or not depending on where its window falls, which is the per-reception randomness
measured. Only three landmarks sit that close to a jump, one of them with a wide interval, and a
difference cannot tell which transmitter rings.

## 2026-09-24: start-of-packet timing at finer resolution, declared before capturing

**Exploratory re-reading of the 2026-09-22 captures** (`2026-09-24-header-timing.py`, and
`bench/analyze_scope_timing.py`, whose first versions had two bugs worth recording: qualification
pooled across files whose packets share sequence numbers, so one run's chains overwrote the
other's; and cluster chaining that let stray pulses glue neighbouring symbols together). With
both fixed, aligned as in the 2026-09-22 review (fitted first preamble pulse, E22 moved back 12
symbols at the nominal 4096 us): the whole header region sits at the same time for both
transmitters within the instrument's spread (-12, +16 and -8 us at -100, 0 and +100 kHz, all
intervals across 0); the SFD downchirp pair moves by -120 to -136 us, the E22's carrier offset.
Three single landmarks differ at one offset each - E22 minus Heltec -56 us at symbol 17.98
(-100 kHz), +34 at 14.44 (0 kHz), -59 at 18.59 (+100 kHz) - on 7-8 captures per transmitter.

**New capture.** The T-Deck, antenna back on, temporarily runs the `tdeck_rssi_scope` probe with its
offset range widened to +/-240 kHz, so a slice at +190 kHz can see the SFD's closing quarter
downchirp (+250 to +125 kHz). Forward direction as now: the Heltec and the E22 (small antennas)
transmit, the first E220 (`/dev/ttyUSB1`) is the delivery oracle. `bench/rssi_capture.py`, scope
offsets -100, 0, +100 and +190 kHz interleaved, 24 captures per transmitter per offset plus the
script's quiet windows, seed 20261012. The captures qualify by the 2026-09-22 criteria (6-pulse
preamble chain at 4096 +/- 100 us in at least 75 % per transmitter and offset, none in quiet
windows).

**Decision**, `bench/analyze_scope_timing.py` (99.5 % bootstrap intervals, because each offset has
about a dozen landmarks):

- *the header is displaced* if the whole-header shift (per capture, mean deviation of header-region
  pulses from the landmark centres; E22 minus Heltec) is at least 10 us with its interval excluding
  0, at two or more offsets;
- *SFD structure differs* if an SFD-window landmark departs from that offset's median SFD shift by
  at least 15 us with its interval excluding it, at two or more offsets - all downchirps move by
  the same carrier offset, so a quarter downchirp that moved differently would mean a different SFD;
- each of the three local differences above is *replicated* if the landmark within 0.1 symbol of
  the same position at the same offset shows the same sign, at least 20 us, interval excluding 0;
- otherwise no displacement at the resolution reached, with the interval widths reported.

The T-Deck's bench image (`v0.1.0-84-gf4dd5c6`) is restored after the capture.

### Result: no displacement; two single-landmark differences reproduce

`2026-09-24-rssi-fine.json.gz` (200 captures, seed 20261012), qualification in
`2026-09-24-rssi-fine.analysis.json` (every offset qualifies: chains in 19-24 of 24 captures per
transmitter, none in the quiet windows), timing in `2026-09-24-rssi-fine.timing.txt`. The first
E220 took the Heltec 21/96 and the E22 94/96 meanwhile. Scope image
`76e0fbfe24e47916fb4c52fcf1c6dcb77479cb178146620aff7b956923c560c8`; the T-Deck is back on its bench
image `v0.1.0-84-gf4dd5c6` afterwards.

| scope offset | whole-header shift, E22 minus Heltec | median SFD shift |
|---|---|---|
| -100 kHz | -4.3 us (-23.2 to 8.4) | -136.6 us |
| 0 | +4.4 us (-17.7 to 25.6) | -127.8 us |
| +100 kHz | -1.3 us (-21.9 to 8.2) | -119.8 us |
| +190 kHz | +20.5 us (-1.8 to 34.2) | -86.3 us |

- **No header displacement and no SFD difference** by the declared rules: our header block sits
  where the E22's does to within about 10-20 us at every offset, and within each offset the SFD
  landmarks move together.
- **Local differences:** -100 kHz, symbol 17.98: E22 minus Heltec **-47.5 us** (interval -69.4 to
  -39.6), replicated; +100 kHz, symbol 18.58: **-63.7 us** (-106.8 to -47.7), replicated; 0 kHz,
  symbol 14.43: +17.3 us (-1.1 to 30.9), not replicated. Two single points where the E22's pulse
  comes 50-60 us (25-30 chips) earlier than ours, now in two independent capture runs.

What that does and does not say. After aligning on the preamble crossing at the same slice, an
upchirp crossing sits at a fixed position whatever the slice, so symbol 17.98 is the same symbol at
every offset - and at 0, +100 and +190 kHz its difference is about +20 us, not -47. A different
symbol value would move every slice alike, so that is not what this is; a difference in the chirp's
time-frequency path localised to part of the band would fit, but so might a pulse-shape artefact
of the filter. Not interpreted further without a model of where each known header symbol should
cross each slice.

Descriptive, not declared: the SFD shift depends on the slice (-137, -128, -120, -86 us from -100
to +190 kHz). A pure carrier offset moves every downchirp crossing equally; a trend across the
band would mean the two transmitters' downchirps differ in slope. With the +190 kHz intervals as
wide as they are (for example -131.6 to -28.4) this is a lead, not a finding.

## 2026-09-24: a weak and a strong E220 on the same packets, declared before running

Two level checks, not used for conclusions. With small antennas on the Heltec and the E22 the gap
stayed (Heltec 6/20 and 1/20 at the two E220s, E22 20/20) and levels did not drop - the E22 at
-43.5 dBm and the Heltec at -48 on the second E220 (`2026-09-24-small-antenna-check.json`). With
the T-Deck's antenna removed it received both at -102 to -104 dBm with SNR 0.25 to 2 dB, all
20/20 of the Heltec's packets and **no header error** (`2026-09-24-tdeck-noantenna-check.json`);
at strong levels today it lost 10-17 % of them, all at the header. Twenty packets do not show
that (p about 0.06), and those SNRs were printed by firmware that dropped the sign between -1 and
0 dB (fixed in `f4dd5c6`). But it is the first condition that has looked like it changes a loss,
and the obvious question follows: does an EBYTE receiver stop losing our packets at a weak level?
The valid power sweep went down to -81 dBm only.

**Bench.** As in the identical-E220 run, forward direction, the Heltec and the E22 with the small
antennas. In the far room the **second E220 has its antenna removed** (weak) and the first keeps
its antenna (strong), 10 cm apart; the T-Deck, also without antenna, runs `v0.1.0-84-gf4dd5c6`.

**Instrument check first**, not used for conclusions: 10 packets per transmitter, to see the weak
E220 still takes the reference and where levels sit.

**Main run.** Heltec ordinary packets and E22 reference, 300 each, palindromic rounds shuffled by
seed 20261010 (run 2: 20261011), gap 0.8 s; the weak E220 queried after every packet
(descriptive levels).

**Decision**, `bench/analyze_level.py`, on the Heltec's packets, paired by packet:

- *level matters* if the weak E220's delivery minus the strong E220's is at least 20 points in
  either direction and an exact McNemar test on the discordant packets gives p < 0.01; claimed
  only if run 2 repeats it;
- otherwise *no effect detected at this sensitivity*, with the difference reported.

**Invalid** if the reference reaches either E220 below 90 % - the weak one must still hear a clean
LoRa signal for its refusals to mean anything - or a board lacks a banner or drops out, or fewer
than 95 % of transmissions are acknowledged.

**Also declared, on the T-Deck:** (1) SNR of Heltec against E22 packets at equal RSSI, mean over
1 dB RSSI bins holding at least 2 of each: *ours lower* if the E22 minus Heltec difference is at
least 1 dB with its 95 % interval above 0; *ours not lower* if the interval's upper end is below
1 dB; otherwise inconclusive. (2) The T-Deck's header-error count and its delivery of the Heltec,
compared descriptively with today's strong-level runs (80-90 %, every miss a header error).

### Result, run 1: level does not matter, our SNR is not lower, and the header loss is not noise

`2026-09-24-level-run1.json`, seed 20261010. Valid: reference 296/300 at the strong E220 and
300/300 at the weak one, 600/600 acknowledged. The instrument check (`2026-09-24-level-check.json`)
had the weak E220 taking the reference 10/10 at -87 dBm.

| Heltec packets, n = 300 | strong E220 (antenna) | weak E220 (no antenna) |
|---|---|---|
| delivered | 44 | 44 |
| level on register 1 | about -40 dBm (earlier runs) | -93 dBm (Heltec), -87 (E22) |

Weak minus strong **0 points**, discordant 32 against 32, McNemar p = 1.0: **no effect detected**;
not repeated. From about -40 to -93 dBm an E220 refuses our packets equally, and its register again
never moved for a lost one (0/256). (Twelve packets reached both, 6.5 expected if independent -
somewhat more than chance; descriptive.)

**SNR at equal RSSI**, T-Deck without antenna: E22 minus Heltec **-0.42 dB** (interval -0.77 to
-0.07) over the bins at -103, -102 and -101 dBm - **ours not lower**; if anything the E22's is
slightly lower. By the one quality measure an SX1262 reports, our signal is not dirtier.

**The T-Deck's header loss does not go away at a weak level, and it is not noise.** The Heltec
arrived at a median -100 dBm and SNR +4.25 dB and was delivered 267/300, with `hdr_err` 33 and
`crc_err` 1. The E22 arrived at -104 dBm and SNR -0.25 dB - four and a half decibels worse - and
was delivered 299/300. The check's 20/20 was chance. Whatever costs our receiver our headers is
not a shortage of signal: it takes packets with more SNR and still fails them at the header, where
it passes the E22's at less. Together with every earlier null, what is left is structural - how
our transmission is built around sync, SFD and header - rather than power, frequency, level or
modulation quality.

## 2026-09-24: two identical E220s on the same packets, declared before running

The pair test that did run (E22 and E220, 5 cm apart) said *near independent*, but the two were
different parts. The second E220 is now beside the remote E220 in the other room, both on the
remote station with the T-Deck (the second E220 at `/dev/ttyUSB0`, CP2102; the first at
`/dev/ttyUSB1`, CH340; the T-Deck at `/dev/ttyACM0`, all identified by boot banner). Placement as
set by the owner; distances to be recorded from the owner's description.

A layout check (5 packets per transmitter, not used for conclusions) gave the E22 5/5 at all
three and the E22 at -43 to -49 dBm and the Heltec at -52 dBm on the second E220's register 1.

**Design.** Forward direction: the Heltec transmits ordinary packets and the E22 is the reference,
both local; the two E220s and the T-Deck receive. 300 packets per transmitter, palindromic rounds
shuffled by seed 20261008 (run 2: 20261009), gap 0.8 s; the second E220 queried after every
packet for its registers (descriptive: radio-level reception of lost packets, and levels).

**Decision**, `bench/analyze_pair.py --arm heltec --a e220 --b e220b --ref e22`, the rules already
declared for the pair question: fewer than 8 deliveries at either - no verdict; *same packets* if
P(B|A) and P(A|B) are both at least 0.6 (and the block-stratified odds ratio at least 3, else
possibly drift); *near independent* if each is at most its marginal plus 0.2; *partial*
otherwise. A second run under the next seed only if run 1 says *same packets* or *partial*.

**Invalid** if the reference reaches either E220 below 90 %, a board lacks a banner or drops out,
fewer than 95 % of transmissions are acknowledged, or the reference's median level on the second
E220 is above -40 dBm. The Heltec itself arrives near -40 dBm at the first E220 (-39 on 2026-09-23),
as in every earlier forward run; noted, not a criterion.

### Result, run 1: identical E220s lose our packets independently

`2026-09-24-e220pair-run1.json`, seed 20261008. Placement, from the owner: the T-Deck and both
E220s lie 10 cm apart with parallel antennas, in a small compartment inside a plasterboard wall
of the far room. Valid: reference 296/300 at the first E220 and 300/300 at the second, reference
median -48 dBm on the second E220's register 1, all 600 transmissions acknowledged.

| Heltec packets, n = 300 | first E220 | second E220 | both |
|---|---|---|---|
| delivered | 38 | 41 | 5 (5.2 expected if independent) |

P(B|A) = 0.13 against P(B) = 0.14, P(A|B) = 0.12 against P(A) = 0.13, block-stratified odds ratio
0.99: **near independent**, so by the declaration not repeated. Two identical EBYTE receivers
10 cm apart do not lose the same transmissions of ours - the same answer the E22 and E220 pair
gave, now without the difference of part.

Also measured:

- **The stage finding holds on the third arrangement.** For all 258 lost Heltec packets with a
  reading, the second E220's register 1 read exactly what it read after the previous packet;
  delivered packets changed it 229/300 (reference) and 37/41 (Heltec), the rest being equal
  values at 1 dB resolution. (The level split was only 5 dB this time, -48 against -53, so the
  earlier midpoint rule was not used.) No lost packet showed AUX or UART activity; the one flagged
  by the script was seq 1, whose window holds the module's boot pulses.
- The T-Deck took 248/300 of the Heltec's packets, all 52 misses header errors, and was
  independent of both E220s (25 both against 31.4; 34 against 33.9).

## 2026-09-24: recalibrating at the working frequency, declared before running

The last cheap constant difference on the list. `configure()` runs Calibrate (0x7F) straight after
the TCXO is set up and *before* SetRfFrequency - so the PLL and ADC are calibrated with the chip
still at its power-on frequency, and only the image is calibrated for 863-870 MHz afterwards.
Semtech's reference driver does the same, so the prior is low; but if the synthesiser's
modulation path depends on a calibration taken 50 MHz away, it would be constant, invisible to
our own configuration-matched receiver's CRC, and at its worst at 500 kHz.

New T-Deck token `cal=0|1`: `cal=1` runs Calibrate with mask 0x3F (RC64k, RC13M, PLL, ADC - not
the image) at the frequency in use, in STDBY_RC just before SetTx, then waits 5 ms; `cal=0` waits
the same 5 ms without calibrating. Both read GetDeviceErrors and log it with any rejection
(`txcal seq=... rejected=... errors=...`).

**Design** as in the series: reverse direction, the T-Deck transmits `ctl` (`cal=0`) and `cal`
(`cal=1`), the remote E220 is the reference, the E22 and the second E220 judge, the Heltec
receives. 200 packets per arm, palindromic rounds shuffled by seed 20261006 (run 2: 20261007),
gap 0.8 s.

**Effect** if D for `cal` differs from `ctl` by at least 20 points either way (the interval is
reported with it); replicated only then. **Valid** as in the series, and only if no `cal=1`
packet was rejected or reported a calibration error (bits 0-3 of GetDeviceErrors).

### Result, run 1: no effect

`2026-09-24-cal-run1.json`, seed 20261006. Valid: reference 198/200 and 200/200; all 200 `cal=1`
packets calibrated with no rejection and GetDeviceErrors 0000, T-Deck image `v0.1.0-80-gf1a0eda`
(`43c7d6ab1a0cf5b9a89c8d0d46d5893bd3f1e5af4dc6cecfa1cdc460f9b62a95`).

| T-Deck arm | E22 | second E220 | D | Heltec |
|---|---|---|---|---|
| `ctl`, 5 ms wait | 28/200 | 32/200 | 0.150 | 168/200 |
| `cal`, Calibrate at 868.125 MHz | 29/200 | 36/200 | 0.163 | 173/200 |

+1.3 points, approximate interval -3.8 to +6.3: **no effect**, not repeated. Recalibrating at the
working frequency does not recover even a few points.

Descriptive: the Heltec's counters show `hdr_err` 52 and, for the first time on this bench,
`crc_err` 6, for 59 missed T-Deck packets.

## 2026-09-23: a series on constant differences, declared before any of it runs

Two EBYTE receivers lose our packets independently, so what separates our transmitter from the
E22 is most likely something constant rather than something that varies per packet. Three
tests, in this order, all without moving a board. Common to all of them:

- **Two EBYTE judges.** Unless stated otherwise the direction is reverse: the T-Deck transmits
  from the other room, the remote E220 is the reference, and the local E22 and second E220
  (5 cm apart) receive, with the Heltec (1 m away) as our own receiver. The primary measure is
  EBYTE delivery pooled over the two modules: D = (E22 + E220b deliveries) / (2 n).
- **Valid** only if the reference reaches each EBYTE receiver at 90 % or more, every receiver
  shows a boot banner, at least 95 % of transmissions are acknowledged and no board drops out.
- **Replication only for an effect.** If run 1 shows an effect by its rule, run 2 repeats it
  under the next seed and the effect is claimed only if run 2 shows it in the same direction.
  If run 1 shows none, it is reported as no effect at this sensitivity and not repeated.

### A. Sync register low nibbles

Receive-side, our SX1262 accepts an E220 with low nibbles 0, 4 and 5 and not with F; transmit-
side only 0, 4 and 5 were ever sent, into an E22. If the low nibbles shape the sync symbols and
an EBYTE receiver is stricter than ours, one value may fit it better.

Arms on the T-Deck, each with the `sync=` token so every arm takes the same standby and register
write before SetTx: `5050`, `5454`, `5858`, `5A5A`, `5C5C`, `5F5F`; plus the reference. 60 packets
per arm, palindromic rounds shuffled by seed 20260929 (run 2: 20260930), gap 0.8 s.

**Effect:** an arm whose D exceeds the `5454` arm's by at least 20 points. An arm 20 or more
points *below* it is reported as the EBYTE receivers' tolerance, not as an effect. The Heltec,
which receives with `5454`, shows our own receiver's tolerance - descriptive.

**Result of A, run 1** (`2026-09-23-sync-run1.json`, seed 20260929). Valid: reference 59/60 at
the E22 and 60/60 at the second E220, 418 of 420 transmissions acknowledged.

| T-Deck sync registers | E22 | second E220 | D | Heltec (receives with 5454) |
|---|---|---|---|---|
| 5050 | 12/60 | 16/60 | 0.23 | 50/60 |
| 5454 (what we send) | 17/60 | 11/60 | 0.23 | 57/60 |
| 5858, 5A5A, 5C5C, 5F5F | 0 | 0 | 0 | 0 |

**No effect**: 5050 matches 5454 exactly, and by the series rule run 1 is not repeated. What the
run does show is that the low nibble is transmitted, not only a receiver setting: every value
with bit 3 set makes the packet undecodable for everyone, our own receiver included, while 0 and
4 pass both EBYTE receivers equally. EBYTE's receivers tolerate the same range ours does.

### B. Regulator mode and the erratum 15.1 bit

Both are set by our drivers and never varied: DC-DC (`SetRegulatorMode 1`) and bit 2 of 0x0889
cleared, as Semtech's erratum 15.1 asks at 500 kHz. What EBYTE's firmware does with either is
unknown.

New T-Deck bench tokens, each applied for one packet and restored after it: `ldo=0|1` (standby,
SetRegulatorMode, back to DC-DC afterwards) and `txmod=0|1` (bit 2 of 0x0889 cleared or set
immediately before SetTx, by read-modify-write, cleared afterwards). Every token packet logs the
two values read back just before SetTx. *Amended before run 1:* the regulator mode has no
readback on this chip, so for `ldo` the evidence is the status of the SetRegulatorMode command;
0x0889 is read back. Arms: control `ldo=0 txmod=0`, `ldo=1 txmod=0`,
`ldo=0 txmod=1`, plus the reference; 100 per arm; seed 20261001 (run 2: 20261002).

**Effect:** an arm whose D differs from the control's by at least 20 points in either direction.
**Also invalid** if any token packet's readback (or, for `ldo`, the command status) does not show
the state asked for.

**Result of B, run 1** (`2026-09-23-txcfg-run1.json`, seed 20261001). Valid: reference 99/100
and 100/100; all 300 token packets read back as asked (no SetRegulatorMode rejection in the
T-Deck's log - `reg_cmd=ok` itself only meant the transfer completed - and 0x0889 = 04 for the
100 `txmod=1` packets and 00 otherwise).

| T-Deck arm | E22 | second E220 | D | Heltec |
|---|---|---|---|---|
| control, DC-DC, bit 2 clear | 24/100 | 29/100 | 0.265 | 91/100 |
| LDO | 21/100 | 19/100 | 0.20 | 89/100 |
| 0x0889 bit 2 set | 22/100 | 22/100 | 0.22 | 84/100 |

**No effect** (-6.5 and -4.5 points against a declared 20), so not repeated. It also refutes the
comment in `sx1262.c` that transmitting at BW500 with bit 2 set "produces a signal ... that no
receiver can demodulate": both EBYTE receivers took 22 % of those packets and the Heltec 84 %.
The Heltec's 36 misses were again all header errors.

### C. Frequency

**C1, a measurement, no decision rule.** The T-Deck, rebuilt with `SX_FREQ_ERROR`, receives 10
packets each from the Heltec, the E22 and the second E220 (all in the other room) and from the
remote E220 (10 cm away - noted, since it is a strong signal). Reported: RegFreqError per source.
The E220s' frequencies have never been measured, which is what the ±40 kHz sweep needs to be
read against.

**Result of C1** (`2026-09-23-freq-c1.json`), RegFreqError at the T-Deck, median of 10 (range):

| transmitter | offset | |
|---|---|---|
| Heltec | -9 Hz (-9 to -5) | 8 of 10 received |
| remote E220 | **+164 Hz** (102 to 203) | 10 cm from the T-Deck, strong signal |
| second E220 | +734 Hz (680 to 773) | |
| E22 | +8100 Hz (8046 to 8116) | |

Both E220s sit within a kilohertz - under one ppm, carrier and symbol clock alike - of our two
boards. Only the E22 is far off. So **frequency and clock rate do not separate an EBYTE
transmitter from ours**: the remote E220 transmits within 170 Hz of us and everyone takes it,
and relative to the second E220 its packets arrive 570 Hz off while ours arrive 734 Hz off -
100 % against about 20 %. Together with the flat ±40 kHz sweep, carrier offset is closed for the
E220 targets, and a ppm-coherent clock offset is too, since the E220 reference has none to speak of.

It also bears on C2 before it runs: our receivers take both E220s, arriving within a kilohertz,
without loss (10/10 each here, 24/24 in the matrix), so a near-zero offset is not what costs our
own receiver its headers. C2 is run as declared; the expectation is *not tied to the offset*.

**C2, carrier offset at our own receiver.** Our receiver loses our packets at the header and never
loses the E22's. The E22 arrives at it about 8.2 kHz off; ours arrive within 36 Hz. A new
`TUNE <Hz>` console command retunes the T-Deck's radio. Forward direction: the Heltec and the E22
transmit from here, each Heltec packet preceded by an E22 packet; the T-Deck receives, tuned in
ABBA blocks of 10 Heltec packets to 0 or to +Δ, Δ the E22's offset from C1 rounded to 100 Hz, so
that the E22 then arrives near zero and ours about Δ off. 200 Heltec and 200 E22 packets; seed
20261003 (run 2: 20261004). The remote E220 receives too, descriptively.

**Decision** on the T-Deck's delivery, by source and tuning, sign-flip test over block pairs:
*follows the offset* if the Heltec's delivery rises by at least 8 points at +Δ **and** the E22's
falls by at least 8 points at +Δ, each with p < 0.05; *not tied to the offset* if neither moves by
8 points; *mixed* otherwise. Valid only if every retune is confirmed and the remote E220 takes the
E22 at 90 % or more. With our receiver losing about 10 %, 8 points is close to the most that can
move; the test is of our receiver, and says something about the EBYTE loss only through that.

**Result of C2, run 1** (`2026-09-23-tune-run1.json`, seed 20261003). Every one of the 21
retunes was confirmed, and the remote E220 took the E22 198/200. **The retune went the other way
from what the declaration assumed.** RegFreqError measured in the run itself:

| source | T-Deck at `TUNE 0` | T-Deck at `TUNE 8100` |
|---|---|---|
| Heltec | -13 Hz | +8170 Hz |
| E22 | +8323 Hz | +16505 Hz |

`TUNE 8100` raised every source's measured offset by about 8.2 kHz, where a TX `foff` of the same
sign had raised it too - so either this register's sign differs between moving the transmitter
and moving the receiver, or the receiver moved down; which is not resolved here. The effect on
the design: our packets did arrive about 8.2 kHz off, the E22's own natural offset, as intended;
the E22 did **not** arrive near zero but 16.5 kHz off.

| T-Deck delivery | `TUNE 0` | `TUNE 8100` |
|---|---|---|
| Heltec | 78/100 | 82/100 |
| E22 | 100/100 | 100/100 |

By the declared rule, **not tied to the offset** (Heltec +4 points, p = 0.59; E22 unchanged). The
half that ran as designed is the informative one: our packets, moved to the offset the E22's
arrive with, are lost at the header just as often (all 40 misses `hdr_err`). The half that did
not - the E22 near zero - is covered by C1, where both E220s arrive within a kilohertz and are
received without loss. Not repeated: no effect, and a corrected-sign run would test only what C1
already shows.

**Series summary.** None of the three constant differences moves anything: sync low nibbles 0
and 4 are equivalent and 8 and up are fatal for every receiver; LDO and the erratum bit change
nothing; the E220 reference transmits within 170 Hz of us, so frequency and clock are not the
difference, and our own receiver's header losses do not follow the carrier offset.

## 2026-09-23: does the listening neighbour cost the E22 our packets? Declared before running

Every target receiver on this bench has had an SX1262 or another module within 10 cm of it, and
that has never been switched off in a controlled way. The one hint: the local E22 took the
T-Deck's packets 6/24 and 5/48 on 2026-09-21 with the Heltec 10 cm away, and 4/200 today with
the second E220 5 cm away - 11/72 against 4/200. Those are different days and the boards were
rearranged in between, so it is a reason to test, not a result.

The second E220 can switch its own radio off: `SLEEP` puts it in M0=M1=1 (the E220's deep
sleep, configuration only), `WAKE` returns it to normal (`BENCH_OBSERVE` build). While it is
asleep its RSSI query and transmission refuse to run, since in that mode a `C0` prefix is a
flash write.

**Question.** Does the E22 deliver more (or fewer) of the T-Deck's packets while the E220 5 cm
away is asleep than while it listens?

**Design.** Reverse direction as in the pair run: the T-Deck transmits ordinary packets, each
preceded by a reference packet from the remote E220; the E22, the second E220 and the Heltec
receive. The second E220 alternates asleep/awake in blocks of 10 T-Deck packets, ABBA over
blocks, the first state drawn from the seed (`--toggle e220b --block 10`), with 1 s after each
switch. 400 T-Deck packets, 400 reference packets, fixed size. No register queries in this run.
Replicated once under a second seed if valid; an effect is claimed only if both runs show it
in the same direction.

**Decision**, `bench/analyze_neighbour.py`: an *effect* if E22 delivery asleep minus awake is at
least 8 points in either direction and a sign-flip test over adjacent block pairs gives
p < 0.05; otherwise *no effect detected at this sensitivity*, with the difference and its
interval reported.

**Invalid** if the reference reaches the E22 below 90 % in either state; if the second E220
receives any reference packet while asleep (the switch did not take) or under 90 % while awake;
if any switch is not confirmed or a board drops out.

**Descriptive only:** the Heltec's delivery by state (it is 1 m away, so a second, weaker
neighbour relation), and the second E220's delivery while awake.

**Instrument check first**, not used for conclusions: 2 blocks of 2 packets to see the switch
take effect on the air.

### Result of run 1: no effect detected, and the hint was run-to-run spread

`2026-09-23-neighbour-run1.json`, seed 20260927; the instrument check is
`2026-09-23-neighbour-check.json` (the switch took: asleep 0/2 references, awake 2/2).

| T-Deck packets | neighbour asleep | neighbour listening |
|---|---|---|
| E22 delivered | 23/200 (11.5 %) | 31/200 (15.5 %) |
| reference at the E22 | 196/200 | 200/200 |
| reference at the neighbour (switch check) | 0/200 | 200/200 |

Valid. Asleep minus awake is **-4 points**, approximate 95 % interval -10.7 to +2.7, sign-flip
p = 0.19 over 20 block pairs: **no effect detected at this sensitivity**. Putting the neighbour to
sleep does not raise the E22's delivery by more than about three points.

The first version of `analyze_neighbour.py` grouped runs of equal state instead of the switching
blocks, so ABBA's adjacent same-state blocks merged and pairs came out 10 against 20 packets
(p = 0.63 then). Corrected to fixed blocks of 10 before anything else was read; the declared
difference and the verdict do not depend on it.

The hint that prompted this was spread between runs: in the same arrangement the E22 took 4/200
an hour earlier and 54/400 here.

Exploratory, not declared for this run: with both EBYTE receivers listening, on the same 200
T-Deck packets the E22 delivered 31 and the new E220 55, 11 of them both against 8.5 expected if
independent (P(B|A) = 0.36, P(B) = 0.28). The Heltec was independent of each (51 both against
48.7; 49 against 49.0) and again lost our packets only at the header (38 `hdr_err`, 39 misses).

### Declared before run 2

Run 2 repeats the design under seed 20260928. In addition, its awake blocks - 200 T-Deck packets
with both EBYTE receivers listening - are the declared test of the pair question above, by the
rules in `bench/analyze_pair.py` restricted to those packets (A = E22, B = new E220; at least 8
deliveries each or no verdict; *same packets* if P(B|A) and P(A|B) are both at least 0.6; *near
independent* if each is at most its marginal plus 0.2; *partial* otherwise).

### Result of run 2: no neighbour effect again, and the two EBYTE receivers fail independently

`2026-09-23-neighbour-run2.json`, seed 20260928. Valid.

| T-Deck packets | neighbour asleep | neighbour listening |
|---|---|---|
| E22 delivered | 48/200 (24 %) | 61/200 (30.5 %) |
| reference at the E22 | 198/200 | 198/200 |
| reference at the neighbour (switch check) | 0/200 | 200/200 |

Asleep minus awake -6.5 points, interval -15.2 to +2.2, p = 0.17: **no effect detected**, as in
run 1. Both runs lean the same way - the E22 slightly *worse* with its neighbour asleep - but
neither reaches the declared threshold, and it is not claimed. What the two runs do bound is the
hypothesis that prompted them: switching a listening module off 5 cm from the target does not
recover the lost packets.

**The declared pair test** (awake blocks, 200 T-Deck packets): the E22 delivered 61, the new E220
36, both 12 against 11.0 if independent; P(B|A) = 0.20 against P(B) = 0.18, P(A|B) = 0.33 against
P(A) = 0.31; Mantel-Haenszel odds ratio 1.16. **Near independent.** Run 1's awake blocks, not
declared, say the same (11 against 8.5).

So two EBYTE receivers 5 cm apart, of the two chip families, do not lose the same transmissions
of ours. Which packets fail is not fixed by the transmission: our signal is steadily marginal
for these receivers and each reception is a fresh draw. That points away from anything that
varies from one of our transmissions to the next (oscillator start, PLL lock, SetTx timing) and
towards a constant difference of configuration or hardware. Two identical E220s would be the
cleaner form of the test; it has not been run.

Also in this run: the Heltec took 361/400 of the T-Deck's packets, lost all 39 misses at the
header (`hdr_err` 39), and was independent of both EBYTE receivers (102 both against 98.4; 31
against 32.6). The E22's share of the T-Deck's packets in the same arrangement today: 2 %, 14 %,
27 % in three consecutive runs, with its reference at 98-100 % each time.

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
