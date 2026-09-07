# Measurements

Every figure here is reproducible from this repository. Where a number is an
estimate rather than a measurement, it is labelled as one.

All results below were produced on an Apple Silicon Mac over loopback, which
exercises the complete protocol and metrics path but not channel contention,
link-layer retries or power-save behaviour. Real-radio figures arrive with M2.

---

## 1. Metrics validated against ground truth

`tools/impair` parses AUDIO headers purely so its event log can name the exact
sequence numbers it discarded. Validating the receiver's counters is therefore a
set comparison rather than a rate comparison.

| Scenario | dropped (truth) | reported | duplicated (truth) | reported |
|---|---|---|---|---|
| burst 10%, mean length 3 | 68 | **68** | 6 | **6** |
| independent 8% | 47 | **47** | 9 | **9** |
| `poor.conf` | 30 | **30** | 3 | **3** |
| `congested.conf` | 0 | **0** | 2 | **2** |

Reproduce:

```bash
build/tools/radiobench/radiobench respond --port 47301 --json > respond.json &
build/tools/impair/impair --listen 47300 --forward 127.0.0.1:47301 \
    --seed 42 --log ground-truth.csv --duration 20 --json \
    burst_loss_percent=10 burst_mean_length=3 duplicate_percent=1 > impair.json &
# wait for `impair: ready` on stderr before starting the sender
build/tools/radiobench/radiobench send --peer 127.0.0.1:47300 \
    --rate 50 --duration 12 --seed 7 --json
```

Then compare the `lost` and `duplicates` fields in `respond.json` against the
non-`forward` verdicts in `ground-truth.csv`, restricted to the observed
sequence span.

### Why the reported reorder count exceeds the applied one

The proxy applied its explicit reorder delay to 8 packets in one run while the
receiver reported 150 reordered. Both are correct. Reordering is implemented as
a consequence of per-packet delay rather than as a shuffle, so heavy-tailed
jitter reorders traffic on its own — with a 50 ms Pareto scale against 20 ms
packet spacing, most packets overtake something. The explicit knob exists to
produce reordering *without* inflating measured jitter, so the two effects can be
studied separately.

---

## 2. Reproducibility

The impairment model is seeded, and the transforms are hand-written rather than
taken from `<random>`'s distributions, which are not portable across standard
library implementations. Over one 301-packet run, comparing the full decision for
every packet — verdict, delay, duplicate flag, reorder flag:

| Comparison | Result |
|---|---|
| seed 42 versus seed 42 | identical at all 301 positions |
| seed 42 versus seed 43 | differs at all 301 positions |

This is the property that makes an A/B comparison attributable. `tc netem`
cannot provide it: two runs with identical settings produce different loss
patterns, so the measured difference between FEC-on and FEC-off would include an
unknown amount of pattern noise with no way to separate it.

---

## 3. Loss structure

Opus in-band FEC reconstructs frame N−1 from frame N's payload, so it repairs an
isolated loss well and a run of consecutive losses not at all. The *structure* of
loss therefore matters as much as its rate, and benchmarking against independent
random loss alone would overstate FEC substantially.

The model is a two-state Gilbert-Elliott chain, parameterised by overall loss
rate and mean burst length. 400,000 packets per row, one seed:

| Model | measured loss | outage events | mean length | worst case |
|---|---|---|---|---|
| independent 5% | 4.969% | 18,892 | 1.05 | 4 |
| burst 5%, mean 2 | 4.886% | 9,896 | 1.98 | 16 |
| burst 5%, mean 4 | 4.896% | 4,876 | 4.02 | 29 |
| burst 5%, mean 8 | 5.101% | 2,476 | 8.24 | **62** |
| burst 1%, mean 6 | 0.973% | 672 | 5.79 | 42 |
| burst 10%, mean 4 | 9.883% | 9,873 | 4.00 | 30 |

Requested rates are hit to within 0.2% and requested mean burst lengths to
within 3%.

Compare rows one and four. Identical 5% loss; 18,892 outages never worse than
four frames versus 2,476 outages reaching 62 frames — **1.24 seconds of audio
lost in a single event**. No amount of FEC addresses that.

Setting mean burst length to 1 degenerates the chain to independent loss, which
gives an A/B control that shares the same code path.

---

## 4. Clock offset, checked against a known answer

Both `radiobench` processes ran on one machine and read one clock, so the true
offset is exactly zero. The estimator has no way to know that.

```
$ radiobench ping --peer 127.0.0.1:47100 --count 40 --interval 25
  probes         40 sent, 40 replied, 0 timed out
  rtt            p50 0.199  p95 0.335  p99 0.447  min 0.150  max 0.447 ms
  clock offset   +0.034 ms +/- 0.080 ms  (peer minus local)
  selected from  rtt 0.160 ms, the lowest of 40 accepted probes
```

+34 µs, inside the ±80 µs bound the estimator derived for itself. The bound is
`rtt/2` of the selected probe, which is the worst-case error if the path's delay
were entirely one-sided.

This is a correctness check rather than a plausibility check: a wrong
four-timestamp formula, or a selection rule that averaged instead of taking the
minimum, would not land inside it.

### Why the minimum-RTT sample is selected rather than averaged

Queueing delay is non-negative and accumulates on one direction of the path at a
time, so it is a bias rather than noise. Averaging suppresses zero-mean error; it
embeds one-sided error, and additional samples do not help.

Seven congested probes (20 ms of outbound queueing) plus one quiet probe, true
offset 5000 µs:

| Estimator | Result |
|---|---|
| minimum-RTT selection | 5000 µs — exact |
| mean of all eight | 13,750 µs — 8.75 ms wrong |

---

## 5. Instrument characterisation

Over loopback, with no network in the path at all, the receiver measured 1.94 ms
mean and 7.27 ms peak jitter. The trace file locates the source:

| Quantity | Value |
|---|---|
| target inter-departure interval | 20,000 µs |
| observed p50 | 19,792 µs |
| observed p95 | 24,100 µs |
| observed range | 15,611 – 25,546 µs |
| standard deviation | 2,229 µs |
| sender's max deviation from the ideal grid | 7,085 µs |
| **receiver's measured peak jitter** | **7,271 µs** |

The last two agree. `nanosleep` guarantees a minimum sleep rather than a
maximum, and overshoots by hundreds of microseconds to a couple of milliseconds
on a general-purpose scheduler, so departures scatter around the 20 ms grid. The
receiver measures that scatter as jitter because from its side it is
indistinguishable from the network having caused it.

Three consequences:

1. **A jitter buffer absorbs sender-side jitter too.** It cannot tell the
   difference, so imprecise send timing costs the receiver real buffer depth,
   which is real latency.
2. **Host-side jitter figures carry roughly 2 ms of instrument noise** and are
   quoted as such throughout this document.
3. **This should largely disappear on device.** Android capture timing comes from
   the audio clock — Oboe delivers a callback when the hardware has 960 samples
   ready, driven by a crystal rather than a scheduler. If device timing turns out
   to be *worse* than 2 ms, that is itself a finding about Android audio
   scheduling, and it is only recognisable because this host baseline exists.

### The pacer under the same conditions

Signed deviation from the ideal 20 ms grid, across a 200-frame run:

| Frame | Deviation |
|---|---|
| 0 | 0 µs |
| 50 | +2,981 µs |
| 100 | +3,832 µs |
| 150 | +3,998 µs |
| 199 | +4,125 µs |

Mean absolute deviation was 2,653 µs over the first half and 2,965 µs over the
second — flat, so the error is bounded rather than accumulating. That is the
drift-free schedule working: departures are computed by advancing a fixed
schedule, not by adding an interval to the current time.

Field data can show the error is bounded but cannot isolate the counterfactual,
because the same sleep behaviour cannot be replayed under a different schedule.
The controlled comparison lives in `core/tests/test_pacer.cpp`, where identical
300 µs-per-frame lateness produces zero drift under the fixed schedule and 30 ms
of accumulated drift under `next = now + interval`.

---

## 6. Wire overhead

| Configuration | Overhead |
|---|---|
| 16-byte header, 80-byte Opus frame | 17% |
| RTP's 12-byte header, same payload | 13% |
| 28-byte header (initial design) | 26% |

The four bytes above RTP buy a 32-bit sequence number, which removes the need
for RTP's rollover-counter machinery. Measured on the wire at 50 packets/second
with an 80-byte payload: 38.6 kbps, matching 96 bytes × 50 × 8 exactly.

---

## 7. Test suite verification

An off-by-one was injected into the media length check — `<=` became `<`, which
admits a header-only datagram carrying no audio. Five independent tests failed,
drawn from three different families:

| Test | Family |
|---|---|
| media length boundaries are enforced exactly | unit |
| validation happens in the order the specification mandates | unit |
| every reject vector is rejected with the specified class | conformance |
| every datagram length from 0 to 64 bytes is handled | adversarial input |
| mutations of valid packets never break a parser | adversarial input |

A test suite that has never failed is a suite of unknown value, so this check is
worth repeating whenever a new layer lands.
