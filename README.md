# Full-Duplex Radio

A low-latency LAN intercom built on a custom UDP protocol — push-to-talk,
voice-activated, then full duplex — with Opus, adaptive jitter buffering, and
latency measured rather than estimated.

[![CI](https://github.com/shubhamDhapola/full-duplex-radio/actions/workflows/ci.yml/badge.svg)](https://github.com/shubhamDhapola/full-duplex-radio/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-macOS%20%7C%20Linux%20%7C%20Android%20%7C%20iOS-lightgrey.svg)

> **Status: milestone 0 of 9 complete.** The protocol, transport, measurement
> layers and host tooling are done and tested. There is **no mobile app yet** —
> Android is M2, iOS is M9. See the [roadmap](ROADMAP.md) for what exists and
> what does not.

---

## What this is

Voice is captured 50 times a second, encoded into ~80-byte Opus frames, and sent
as UDP datagrams with no retransmission and no delivery guarantee. The receiver
holds them for a few tens of milliseconds — long enough to absorb variation in
arrival time, short enough that the conversation still feels live — then decodes
and plays them on a clock it does not control.

Everything difficult about the project follows from those two sentences: the
network is late and lossy by nature, the audio device demands a frame every 20 ms
without exception, and the two are driven by different clocks that drift apart.

**TCP cannot do this.** One lost packet blocks every packet behind it until the
retransmit completes, so a single loss damages four frames instead of one and
adds a round trip of latency to everything after it. In real-time media a frame
that arrives late is worth no more than one that never arrives — so this uses
UDP and repairs loss at the codec layer, where repair is cheap and local.

### Why not WebRTC

A fair question, answered at length in
[ADR 0001](docs/decisions/0001-custom-udp-transport-not-webrtc.md). The short
version: most of WebRTC's complexity is NAT traversal for the open internet,
which does not exist on a single subnet — and the components that remain
(NetEq's adaptive jitter buffer, loss concealment, FEC) are precisely the parts
worth building here. The ADR also records the conditions under which that
decision should be reversed.

---

## Results

Everything below is reproducible from this repository. No numbers are estimated.

### Metrics validated against ground truth

The impairment proxy parses AUDIO headers purely to log *which* sequence numbers
it dropped, so validating the receiver's counters is an exact set comparison
rather than a rate comparison:

| Scenario | dropped (truth) | reported | duplicated (truth) | reported |
|---|---|---|---|---|
| burst 10%, mean length 3 | 68 | **68** | 6 | **6** |
| independent 8% | 47 | **47** | 9 | **9** |
| `poor.conf` | 30 | **30** | 3 | **3** |
| `congested.conf` | 0 | **0** | 2 | **2** |

Not "3.05% versus 3.1%, close enough" — exactly 68 and exactly 68.

### Clock offset, checked against a known answer

Both `radiobench` processes run on one machine and read one clock, so the true
offset is exactly zero. The estimator, which has no way to know that, reports:

```
$ radiobench ping --peer 127.0.0.1:47100 --count 40 --interval 25
  probes         40 sent, 40 replied, 0 timed out
  rtt            p50 0.199  p95 0.335  p99 0.447  min 0.150  max 0.447 ms
  clock offset   +0.034 ms +/- 0.080 ms  (peer minus local)
```

+34 µs against a self-derived ±80 µs bound. A wrong four-timestamp formula or a
broken minimum-RTT selection would not land inside it.

### Loss structure, not just loss rate

Opus in-band FEC reconstructs frame N−1 from frame N, so it repairs an isolated
loss well and a run of losses not at all. Benchmarking against independent
random loss alone would therefore overstate it substantially. 400,000 packets
per row:

| Model | measured loss | outage events | mean length | worst case |
|---|---|---|---|---|
| independent 5% | 4.969% | 18,892 | 1.05 | 4 frames |
| burst 5%, mean 8 | 5.101% | 2,476 | 8.24 | **62 frames** |

Same loss rate. A 62-frame burst is 1.24 seconds of audio gone at once.

### An instrument artefact, quantified

On loopback — no network at all — the receiver measured 7.3 ms of peak jitter.
The trace file located it in our own sender: `nanosleep` overshoots, so
departures scatter around the 20 ms grid.

```
sender's max deviation from the ideal grid   7085 µs
receiver's measured peak jitter              7271 µs
```

Host-side jitter figures therefore carry ~2 ms of instrument noise and are
quoted as such. Full write-up in [docs/measurements.md](docs/measurements.md).

---

## Quick start

Requires CMake ≥ 3.24, Ninja, and a C++20 compiler.

```bash
brew install cmake ninja          # macOS
# apt install cmake ninja-build   # Debian/Ubuntu

cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

159 tests, warning-free under `-Wconversion -Wsign-conversion -Wold-style-cast`.

### Try the transport

Three terminals. The proxy sits between the two endpoints and applies a
reproducible impairment pattern.

```bash
# 1. a peer that answers PING and accounts for received streams
build/tools/radiobench/radiobench respond --port 47001

# 2. a lossy link: 5% loss in bursts of ~5, 50 ms delay, heavy-tailed jitter
build/tools/impair/impair --listen 47000 --forward 127.0.0.1:47001 \
    --scenario benchmarks/scenarios/poor.conf --seed 42 --log ground-truth.csv

# 3. a paced 50 packets/second stream
build/tools/radiobench/radiobench send --peer 127.0.0.1:47000 \
    --rate 50 --duration 10 --seed 7 --trace send.csv
```

Terminal 1 reports loss, duplicates, reordering and jitter. `ground-truth.csv`
records what the proxy actually did to every packet, so the two can be compared
exactly. **The same seed replays a byte-identical impairment pattern** — which
is what makes an A/B comparison attributable to the change under test rather
than to a different loss pattern, and the main reason this exists instead of
`tc netem`.

### Measure round-trip time and clock offset

```bash
build/tools/radiobench/radiobench ping --peer 192.168.1.14:47000 --count 100
```

### Run under sanitizers

Several tests are meaningless without them — the unaligned-load tests pass on
x86 and arm64 even when the implementation is undefined behaviour, so only
`-fsanitize=undefined` makes them a proof.

```bash
cmake -B build-san -G Ninja -DRADIO_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-san && ctest --test-dir build-san
```

### Cross-compile the core

The core has no platform dependencies beyond POSIX sockets, and both mobile
targets are kept building from day one so portability cannot rot before the apps
exist.

```bash
# Android
cmake -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
cmake --build build-android

# iOS
cmake -B build-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
cmake --build build-ios
```

---

## Architecture

One C++20 core with no platform dependencies, consumed by three thin layers.
That boundary is what lets the host benchmarks measure *the same code* that will
run on the phone, rather than a second implementation that happens to agree.

```
        ┌──────────────────── platform layer (thin) ────────────────────┐
        │  Android: Kotlin/Compose + Oboe    iOS: Swift/AVAudioEngine   │
        │  UI · audio I/O · permissions · discovery · routing           │
        └───────────────────────────┬───────────────────────────────────┘
                        JNI  ───────┴─────── C ABI
        ┌───────────────────────────┴───────────────────────────────────┐
        │                core/  —  C++20, no platform deps              │
        │  proto · net · time · metrics · jitter · audio · session       │
        └───────────────────────────┬───────────────────────────────────┘
                                    │ same code, host build
        ┌───────────────────────────┴───────────────────────────────────┐
        │  tools/radiobench  (protocol peer)   tools/impair  (proxy)     │
        └───────────────────────────────────────────────────────────────┘
```

`radiobench` is a full protocol peer rather than a test stub, so once the
Android app exists you can run phone ↔ Mac with a debugger, `tcpdump` and
complete tracing on one end of a real call.

More in [docs/architecture.md](docs/architecture.md).

### Layout

```
protocol/          normative wire-format spec + conformance vectors
core/
  include/radio/   public headers
  src/             proto · net · time · metrics · sim
  tests/           159 tests: unit, conformance, adversarial input
tools/
  radiobench/      protocol peer: respond · ping · send
  impair/          seeded impairment proxy with ground-truth logging
benchmarks/        impairment scenarios covering the test matrix
docs/
  architecture.md
  measurements.md  results and instrument characterisation
  decisions/       architecture decision records
```

---

## The protocol

Defined normatively in [protocol/specification.md](protocol/specification.md).
A 16-byte media header — four bytes wider than RTP, in exchange for a 32-bit
sequence number that removes the need for RTP's rollover-counter machinery:

```
 off sz  field
  0   1  version
  1   1  type
  2   2  flags          talkspurt start/end, FEC, DTX; 12 bits reserved
  4   4  stream_id      one continuous stream from one source
  8   4  sequence       per-stream, random start, wraps
 12   4  timestamp      48 kHz sample units, per-stream
 16   -  payload        one Opus packet; length = datagram length - 16
```

There is no length field, because a UDP datagram already carries its own — a
second copy could only ever disagree with the first. Unknown versions, types and
reserved flag bits all **fail closed**: a receiver that ignored an unrecognised
flag could feed ciphertext to the decoder or duplicate audio, and the bug would
surface as an artefact the user hears instead of a counter someone can read.

### Conformance

[`protocol/testvectors/`](protocol/testvectors/) holds 35 vectors generated by
[`generate.py`](protocol/testvectors/generate.py), which is an **independent
implementation of the wire format written from the specification** rather than
from the C++ encoder. A fixture generated by the code under test can detect
regressions but never original misunderstandings; two implementations written
separately from one document agreeing is evidence the document is unambiguous.

Every port — C++, Android/JNI, iOS/Swift — must pass the full set. Cross-platform
interoperability is established by that, not by two devices appearing to work
when placed side by side.

---

## Testing

| Layer | What it establishes |
|---|---|
| Unit tests | arithmetic, against hand-computed expectations |
| Conformance vectors | the wire format matches an independent reading of the spec |
| Adversarial input | the parser survives hostile bytes; exhaustive over all 65536 flag values |
| Ground-truth validation | reported metrics match what the impairment proxy actually did |
| Sanitizers | ASan + UBSan clean; several tests are only meaningful under them |

The suite is itself verified. Injecting an off-by-one into the media length
check (`<=` became `<`, admitting a header-only datagram) failed five
independent tests across three families — two unit tests, the conformance
vectors, and both adversarial-input tests. A suite that has never failed is a
suite of unknown value.

---

## Roadmap

M0 is complete. [ROADMAP.md](ROADMAP.md) has the detail.

| | Milestone | Status |
|---|---|---|
| M0 | Protocol, transport, measurement, host tooling | **complete** |
| M1 | Opus, lock-free ring buffers, pull-model jitter buffer | next |
| M2 | Android: Oboe, JNI, push-to-talk, diagnostics | |
| M3 | mDNS discovery, sessions, reconnection | |
| M4 | Adaptive jitter target, clock-drift compensation, adaptive FEC | |
| M5 | Full duplex, multi-speaker mixing | |
| M6 | Voice activity detection | |
| M7 | Acoustic echo cancellation | |
| M8 | AEAD media encryption, replay protection | |
| M9 | iOS | |

---

## Licence

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for dependency licences.
