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

---

## 8. The media pipeline, end to end

Produced by `benchmarks/run_matrix.py`, which runs `radiobench wavloop` across
every scenario in `benchmarks/scenarios/` with FEC off and on. 8 s fixture,
seed 42, 60 ms jitter buffer target, 32 kbps, `--expected-loss 20` on the FEC
runs. The whole sweep takes about 100 seconds and exits non-zero if any cell
was not a valid measurement.

```bash
benchmarks/run_matrix.py --seed 42 --seconds 8
```

Both ends of this pipeline run in one process and read one clock, so
capture-to-playout is a subtraction with no clock-offset estimator in it. That
is the entire reason M1 measures on the host before M2 touches a phone: this
figure has no error bar, and every on-device figure will be compared against it.

### 8.1 Per-stage latency budget

From the `excellent` scenario, FEC off. Milliseconds.

| Stage | p50 | p95 | p99 | max |
|---|---|---|---|---|
| capture → encode | 0.32 | 0.51 | 0.67 | 2.02 |
| queue → sent | 0.04 | 0.11 | 0.14 | 0.21 |
| sent → received (loopback) | 0.08 | 0.16 | 0.31 | 2.42 |
| network hold (modelled) | 0.00 | 3.07 | 4.22 | 5.18 |
| jitter buffer | 79.87 | 81.92 | 81.92 | 85.47 |
| decode | 0.06 | 0.12 | 0.15 | 0.61 |
| **end to end** | **81.92** | **81.92** | **81.92** | **88.07** |

**The codec is 0.4% of the latency.** Encode is 320 µs and decode 60 µs against
a 20 ms frame. Every millisecond that matters is buffer depth, which is a
deliberate choice, not a cost imposed by the codec.

**A 60 ms target produced 82 ms, and that is correct.** Playout happens only
when the consumer asks, every 20 ms. A frame's deadline lands wherever the
anchor put it and then waits for the next tick, so the effective delay is the
target rounded up to a whole frame. *The jitter buffer target is a lower bound.*
Quote the 82.

Percentiles come from the log-linear histogram of section 3, so they are bucket
upper bounds with 3.1% worst-case relative error and never understate. Read
81.92 as "about 80 ms".

### 8.2 Loss recovery across the matrix

400 frames per cell. `dropped` is what the model discarded; `late` is what
arrived after its playout moment.

| Scenario | FEC | dropped | late | from packet | FEC recovered | concealed | e2e p50 |
|---|---|---|---|---|---|---|---|
| excellent | off | 0 | 0 | 400 | 0 | 0 | 81.9 |
| excellent | on | 0 | 0 | 400 | 0 | 0 | 81.9 |
| normal | off | 0 | 0 | 400 | 0 | 0 | 81.9 |
| normal | on | 0 | 0 | 400 | 0 | 0 | 81.9 |
| asymmetric | off | 0 | 0 | 400 | 0 | 0 | 120.8 |
| asymmetric | on | 0 | 0 | 400 | 0 | 0 | 120.8 |
| congested | off | 0 | 28 | 368 | 0 | 32 | 163.8 |
| congested | on | 0 | 28 | 368 | **29** | 3 | 163.8 |
| poor | off | 12 | 34 | 349 | 0 | 52 | 241.7 |
| poor | on | 12 | 34 | 349 | **36** | 17 | 241.5 |
| extreme | off | 25 | 40 | 329 | 0 | 73 | 401.4 |
| extreme | on | 25 | 40 | 329 | **48** | 28 | 401.4 |

`asymmetric` reduces to its upstream direction here: media travels one way, so
the return path it exists to characterise has nothing to act on.

### 8.3 The A/B comparison, and its control

The acceptance criterion is not "FEC sounds better". It is that two runs over an
*identical* impairment pattern differ, and that the FEC counter accounts for the
difference. The harness verifies the patterns really were identical — same
`considered`, `forwarded`, `dropped`, `duplicated`, `reordered` — and refuses to
report a pair where they were not.

| Scenario | concealed off → on | recovered | samples differing | mean abs error |
|---|---|---|---|---|
| excellent | 0 → 0 | 0 | 97.4% | 1260 |
| normal | 0 → 0 | 0 | 97.1% | 1257 |
| asymmetric | 0 → 0 | 0 | 96.4% | 1248 |
| congested | 32 → 3 | 29 | 96.2% | 3812 |
| poor | 52 → 17 | 36 | 95.7% | 3571 |
| extreme | 73 → 28 | 48 | 94.5% | 3896 |

**The first three rows are the control, and they are the most useful rows in the
table.** Nothing was lost, nothing was recovered, and the outputs still differ
by ~1255 mean absolute sample error against a signal of amplitude 12000.

That is the *cost* of FEC. Enabling it makes Opus spend part of a fixed 32 kbps
on redundancy, so the primary encoding is worse whether or not the redundancy is
ever needed. Without this control, the 3571 on `poor` reads as "FEC improved the
audio by 3571" when roughly a third of it is FEC having degraded it. Subtract
the control before crediting the benefit.

**FEC recovers lateness, not just loss.** `congested` lost nothing on the wire
and still had 29 frames rebuilt from redundancy: 28 packets arrived after their
playout moment. A late packet and a lost one are the same hole to the jitter
buffer, so the same mechanism repairs both — which is also why `late` and
`dropped` have to be counted separately to interpret any of this.

**Why 36 of 48 and not all of them.** `poor` drops 5% in bursts of mean length
5. Opus carries frame N−1's redundancy inside packet N, so a burst destroys the
copy along with the original and only the first frame of each run is
recoverable. This is the measured version of the argument in section 3 for
modelling burst loss rather than independent loss: benchmarked against
independent 5% loss, FEC would have looked close to perfect.

### 8.4 A fixed buffer inherits its first packet's bad luck

`poor` and `extreme` use Pareto jitter, and they report 242 ms and 401 ms
end-to-end against a 60 ms target. The jitter buffer is not misbehaving.

The playout anchor is placed when the first packet of a talkspurt arrives. If
that packet draws a heavy-tailed delay — 180 ms on `poor`, 340 ms on `extreme` —
the anchor is placed that late and **every subsequent frame of the talkspurt
inherits it**. Nothing in M1 recovers from it.

Note what it does to the other counters: a late anchor makes the buffer
*effectively deeper*, so `late` falls. A latency regression that improves a
quality counter is only visible if the two are read together, which is the
argument for reporting them side by side rather than reducing them to a score.

This is the concrete case for M4's adaptive jitter target, re-anchoring at
talkspurt boundaries where a resize costs no artefact.

### 8.5 What is not measured here

- **One direction only.** wavloop sends one way. Duplex mixing is M5.
- **One talkspurt.** The fixture is continuous speech, so re-anchoring and DTX
  resumption are exercised by unit tests rather than by this matrix.
- **Host scheduling, not device audio.** The playout clock here is the pacer,
  not a hardware callback. Section 5's ~2 ms of instrument noise applies.
- **`late` is not seeded.** Network counters replay exactly; `late` depends on
  when the process was scheduled, and moved by one frame in 400 across repeated
  full-matrix runs. Treat single-frame differences in `late`, `concealed` and
  `recovered` as noise, and the network columns as exact.

---

## 9. The device audio path

First figures from hardware: a Redmi Note 9 Pro, arm64-v8a, Android 12, with
the app's microphone-to-speaker loopback. Oboe 1.9.3 over AAudio.

These characterise the *audio device*, not the pipeline. End-to-end
mouth-to-ear against the 82 ms host baseline of section 8 needs the transport
on the phone, and arrives later in M2.

### 9.1 The capture chain decides whether you get low latency at all

Requesting `PerformanceMode::LowLatency` and `SharingMode::Exclusive` on both
streams, and varying only the input preset:

| Input preset | burst | buffer | performance mode | sharing |
|---|---|---|---|---|
| `VoiceCommunication` | 960 (20 ms) | 2880 | **None** | Shared |
| `VoiceRecognition` | **96 (2 ms)** | 192 | **LowLatency** | **Exclusive** |
| `Unprocessed` | **96 (2 ms)** | 192 | **LowLatency** | **Exclusive** |

Output was identical in all three: 192-frame bursts (4 ms), LowLatency, Shared.

`VoiceCommunication` asks the platform for its voice-processing chain — on most
devices hardware echo cancellation and noise suppression. **On this device you
cannot have that and the low-latency capture path at the same time.** The cost
is a tenfold increase in capture burst, 20 ms against 2 ms, plus the loss of
exclusive mode.

That is a finding for M7, not for M2. The echo-cancellation milestone has to
decide whether to adopt the platform's canceller or build one, and this is the
price of the first option stated in milliseconds rather than in principle. The
preset is therefore a runtime choice in `AudioEngine`, not a constant, so the
comparison can be re-run on every device that matters.

It is also a warning about the word "requested". `PerformanceMode::LowLatency`
is a request; what came back is read with `getPerformanceMode()` and reported.
A build that assumed the request was granted would have shipped 20 ms capture
bursts and called them low latency.

### 9.2 Stream properties on the low-latency path

`VoiceRecognition` capture, 8-second run, 4075 input and 2133 output callbacks:

| Quantity | Value |
|---|---|
| input burst / buffer | 96 / 192 frames (capacity 3072) |
| output burst / buffer | 192 / 384 frames (capacity 1536) |
| output latency, Oboe's estimate | 29.7 ms |
| ring occupancy, steady state | 288 samples (6.0 ms) |
| xruns, input / output | 0 / 0 |
| ring overflows / underruns | 0 / 0 |
| worst observed gap between output callbacks | 98.5 ms |

The worst-gap figure is a maximum over the run, not a typical value, and it was
observed while the device was also servicing `adb` and a screen capture. It is
recorded because a gap that exceeds the 4 ms burst is the audio deadline being
missed, and the xrun counter did not report it — two instruments disagreeing is
worth knowing before either is trusted on its own.

### 9.3 A start-up ordering cost that no counter called a fault

Capture starts before playback, so that the first output callback has something
to play instead of opening with underruns that are really just ordering.

Opening the output stream took 141 ms on this device, and every sample captured
during that window sat in the ring for the rest of the session:

| | ring occupancy |
|---|---|
| capture started first, no correction | 5760 samples — **120 ms** |
| after priming | 288 samples — **6 ms** |

120 ms of standing latency, on a pipeline whose entire host budget is 82 ms,
and nothing was wrong: no xrun, no overflow, no underrun. Every health counter
read zero while the app was twice as slow as its own design target.

The fix is to discard the backlog on the first output callback, keeping one
cushion burst. It happens in the callback rather than in `start()` because
`discard()` advances the read index, and the read index belongs to the
consumer — doing it from the starting thread would make the ring a two-reader
structure and forfeit the lock-free argument entirely.

**The general lesson is the one worth keeping: latency does not have to come
from a fault.** A buffer that is merely *fuller than it needs to be* costs
exactly as much as one that is too small costs in glitches, and only one of
those has a counter watching it.

### 9.4 The pipeline on the device

The whole media path running on the phone, with the peer set to `127.0.0.1` so
the datagrams go out of the socket and come back in. That exercises the socket,
the packet codec, Opus, the reorder queue and the jitter buffer with real audio
at both ends; only the LAN hop is absent.

Five seconds of push-to-talk, `VoiceRecognition` capture, 60 ms jitter target,
FEC off:

| Quantity | Value |
|---|---|
| sent | 248 packets, 20 KiB, 1 talkspurt |
| received | 248, 0 rejected |
| played from packet | 244 (4 still in the buffer) |
| late / concealed / FEC-recovered | 0 / 0 / 0 |
| buffer depth | 4 frames — **80 ms** |

The buffer depth is the same 80 ms the host measured in section 8.1 from a 60 ms
target, for the same reason: playout is frame-granular and a deadline landing
mid-tick waits for the next one. The two implementations agreeing on a figure
neither was tuned to produce is the strongest evidence so far that the phone is
running the same pipeline the benchmarks describe.

After the talkspurt closed, 303 received and 303 played with **0 concealed** —
TALKSPURT_END draining to silence rather than concealment, on the device.

### 9.5 A codec cost that was mostly not the codec

The session reported Opus encode at **5342 µs mean, 12508 µs max**, against
280 µs on the host. Six times slower hardware does not explain twenty times
slower encoding.

Timing the same encode at start-up, on an otherwise idle thread, settles it:

| Measurement | Encode |
|---|---|
| start-up self-check, idle thread | **750 µs** |
| inside a running session | **5342 µs mean** |

`now_us()` is wall time, not CPU time. The 750 µs is what the encoder costs on
this device — 3.75% of a 20 ms frame. The other 4.6 ms was the network thread
being descheduled mid-encode while two audio callbacks and the UI competed for
the same cores.

Both numbers are true and only one of them is about Opus. Quoting the in-session
figure as "the codec is slow on ARM" would have sent the next person optimising
the wrong thing entirely — the same trap as the 22 ms loopback in section 8, in
a different disguise.

The fix is scheduling, not a faster encoder: the network thread now asks for
audio-adjacent priority via `setpriority`. **That change is built but not yet
re-measured on the device**, so no post-fix figure is quoted here.
