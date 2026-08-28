# Progress

Low-latency LAN voice intercom: push-to-talk → voice-activated → full duplex,
Android and iOS, over a custom UDP protocol with Opus, adaptive jitter
buffering, and measured latency.

**Status:** M0 complete on loopback. Only the two-machine Wi-Fi run is outstanding, and it is blocked on hardware. M1 is next.

| | |
|---|---|
| Tests | 159 passing, zero warnings under `-Wconversion -Wsign-conversion -Wold-style-cast` |
| Sanitizers | clean under ASan + UBSan |
| Language | C++20 (core), Kotlin (Android, M2), Swift (iOS, M9) |
| Committed | ~11,000 lines across core, protocol spec, decisions |

## Verify

```bash
# Host build and tests
cmake -B build -G Ninja
cmake --build build && ctest --test-dir build --output-on-failure

# Under sanitizers (required for the alignment tests to mean anything)
cmake -B build-san -G Ninja -DRADIO_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-san && ctest --test-dir build-san
```

Prerequisites: `brew install cmake ninja`.

---

## M0 — Transport lab

Goal: two hosts exchanging packets over a real network through a reproducible
impairment proxy, with loss, reordering, jitter and RTT reported and **validated
against the proxy's ground truth**.

### Done

- [x] Repo skeleton, top-level CMake — C++20, strict warnings, opt-in ASan/UBSan/TSan
- [x] Catch2 v3.7.1 pinned via FetchContent
- [x] `protocol/specification.md` — protocol v1, normative, 8 sections
- [x] `radio/bytes.hpp` — big-endian load/store, UB-free at any alignment
- [x] `radio/serial.hpp` — RFC 1982 serial-number arithmetic
- [x] `radio/proto.hpp` + `proto.cpp` — media and control encode/decode, fail-closed validation
- [x] `radio/clock.hpp` — `CLOCK_MONOTONIC_RAW`, sample-unit conversions
- [x] `radio/seq_tracker.hpp` — sliding-window bitmap: loss, duplicate, reorder, too-old
- [x] `radio/jitter_estimator.hpp` — RFC 3550 interarrival jitter, talkspurt re-anchoring
- [x] `radio/histogram.hpp` — log-linear buckets, 3.1% bounded error, exact min/max/mean
- [x] `radio/endpoint.hpp` — address+port value type, no socket headers in the interface, numeric-only parsing
- [x] `radio/udp_socket.hpp` — RAII owning handle, move-only, non-blocking, immediate arrival timestamp, MSG_TRUNC detection
- [x] `radio/pacer.hpp` — drift-free departure schedule with catch-up resync
- [x] `radio/clock_sync.hpp` — NTP-style offset/RTT, min-RTT selection, uncertainty bound, coarse drift
- [x] `radio/trace.hpp` — per-packet stage timestamps, fixed-capacity ring, CSV/JSONL sinks
- [x] `tools/radiobench` — `respond` / `ping` / `send`, human + JSON reports, seeded runs
- [x] `radio/impairment.hpp` — seeded model: Gilbert-Elliott burst loss, 3 jitter shapes, duplication, reordering, token bucket
- [x] `tools/impair` — bidirectional relay, per-direction impairment, ground-truth event log
- [x] `benchmarks/scenarios/` — 6 scenarios covering the benchmark matrix
- [x] `protocol/testvectors/` — 35 vectors from an **independent** Python encoder written from the spec
- [x] Conformance test — accept fields, reject classes, and byte-exact re-encoding
- [x] `test_parser_stress.cpp` — portable seeded random + mutation, exhaustive over lengths, prefixes and all 65536 flag values
- [x] `fuzz_proto.cpp` — libFuzzer entry point, gated (AppleClang ships no libFuzzer, so it warns rather than failing)
- [x] **M0 acceptance criterion met** — reported metrics match the proxy's ground-truth log exactly
- [x] `docs/decisions/0001` — custom UDP transport instead of WebRTC

### Verified working

```
$ radiobench ping --peer 127.0.0.1:47100 --count 40 --interval 25
  probes         40 sent, 40 replied, 0 timed out
  rtt            p50 0.199  p95 0.335  p99 0.447  min 0.150  max 0.447 ms
  clock offset   +0.034 ms +/- 0.080 ms   <- true offset is 0 (same machine)

$ radiobench send --peer 127.0.0.1:47101 --rate 50 --duration 4 --seed 42
  actual rate    49.95 pps       wire bitrate 38.6 kbps
  received 201 of 201, 0 lost, 0 duplicate, 0 reordered
```

The offset check is real validation, not plausibility: both processes share one
clock, so the true answer is exactly 0, and the estimator landed inside its own
stated error bar.

### M0 acceptance: metrics validated against ground truth

The proxy parses AUDIO headers purely to log which sequence numbers it dropped,
so validation is an exact set comparison rather than a rate comparison:

```
                      lost gt   reported   dup gt   reported
burst 10% x3               68         68        6          6   PASS
independent 8%             47         47        9          9   PASS
poor.conf                  30         30        3          3   PASS
congested.conf              0          0        2          2   PASS
```

Determinism: same seed reproduces identical decisions for all 301 packets of a
run; a different seed differs at all 301 positions.

Burst model, 400k packets per row:

```
model                     loss%   events  mean len  max len
independent 5%           4.969%    18892      1.05        4
burst 5%, mean 8         5.101%     2476      8.24       62
```

Same 5% loss, a 62-frame worst case (1.24 s of audio) versus 4. This is why
FEC benchmarked only against independent loss would be misleading.

### Test suite is itself verified

An off-by-one was deliberately injected into the media length check
(`<=` became `<`, so a header-only datagram would be accepted). Five independent
layers caught it:

```
 19 - media length boundaries are enforced exactly          (unit)
 20 - validation happens in the order the spec mandates     (unit)
152 - every reject vector is rejected with the right class  (conformance)
154 - every datagram length from 0 to 64 bytes is handled   (stress)
158 - mutations of valid packets never break a parser       (stress)
```

A suite that has never failed is a suite of unknown value. See `learn/12`.

### Open finding: the host tools add ~2 ms of jitter

`nanosleep` overshoots by hundreds of microseconds to ~2 ms, so departures
scatter around the 20 ms grid and the receiver measures that as jitter:

```
sender max deviation from ideal grid   7085 us
receiver measured peak jitter          7271 us    (same number)
```

Consequences: host-side jitter measurements carry ~2 ms of instrument noise and
must be quoted as such; and this should largely vanish on Android, where capture
timing comes from the audio clock rather than a scheduler. If the device is
*worse* than 2 ms, that is itself the finding. See `learn/10`.

### Remaining

- [ ] **Mac ↔ Mac over real Wi-Fi** — *blocked: needs a second machine.* Everything so far is
      validated on loopback, which exercises the full protocol and metrics path but not
      contention, retries, or power-save. The Android work in M2 supplies the second
      endpoint, so this folds into phone ↔ Mac testing rather than waiting.

---

## M1 — Media pipeline (host)

Goal: wav → Opus → UDP → impairment → jitter buffer → decode → wav, entirely on
one host so **end-to-end latency is exactly measurable with zero clock-sync
error**. That number becomes the reference every device measurement is compared
against.

- [ ] `cmake/opus.cmake` — libopus pinned by URL + SHA256 via FetchContent
- [ ] `radio/opus_codec.hpp` — encoder/decoder wrappers, VOIP mode, 48 kHz mono, 20 ms, 32 kbps; inband FEC, loss hint, DTX, complexity exposed; fully preallocated
- [ ] `radio/ring_buffer.hpp` — lock-free SPSC PCM ring and packet-slot ring, bounded, drop-oldest on overflow
- [ ] `radio/packet_pool.hpp` — fixed-MTU slots, zero steady-state allocation
- [ ] `radio/reorder_queue.hpp` — restore sequence order within a deadline
- [ ] `radio/jitter_buffer.hpp` — **pull model**, decode inside the consumer; returns `{OK, CONCEALED, FEC_RECOVERED, UNDERRUN}`; fixed 40 ms target initially
- [ ] Packet-loss concealment via `opus_decode(NULL)`
- [ ] FEC recovery via `decode_fec=1` on the following packet
- [ ] Late-packet accounting — counted and discarded, never played
- [ ] `radio/wav.hpp` — minimal wav read/write for fixtures and output
- [ ] `tools/radiobench wavloop` — full pipeline + per-frame trace CSV
- [ ] `benchmarks/scenarios/*.toml` — the 5-scenario matrix, versioned
- [ ] `benchmarks/run.py` — drives the matrix unattended
- [ ] `benchmarks/analyze.py` — P50/P95/P99 E2E, underruns, concealments, FEC recoveries → markdown table
- [ ] `docs/latency-model.md` — per-stage breakdown with measured numbers
- [ ] **M1 acceptance:** matrix runs unattended; at 5% burst loss, FEC-on vs FEC-off output wavs from an *identical* impairment pattern are audibly different and the FEC-recovery counter explains why

---

## M2 — Android

Goal: a real phone talking to `radiobench` on the Mac, then phone ↔ phone.
Testing against the host peer first means a debugger, `tcpdump` and full tracing
sit on one end of every call.

- [ ] Gradle project, `minSdk 26`, Kotlin, Jetpack Compose
- [ ] CMake `externalNativeBuild` linking `radio_core` for arm64-v8a + x86_64
- [ ] Oboe via Maven prefab (`com.google.oboe:oboe`)
- [ ] JNI bridge — command/state in, PCM through the core, no JNI on the audio callback
- [ ] Oboe capture and playback callbacks, low-latency performance mode
- [ ] Microphone permission flow
- [ ] PTT UI — hold-to-talk, peer list, connection state
- [ ] Diagnostics screen — RTT, jitter, loss, late, reordered, buffer depth, encode/decode times
- [ ] Conformance test on device against `protocol/testvectors/`
- [ ] Phone ↔ Mac, then phone ↔ phone
- [ ] Device audio-latency measurement, compared against the M1 host baseline

---

## M3 — Discovery and session

- [ ] Android NSD / Bonjour, service type `_fdradio._udp.local`
- [ ] `JOIN` / `JOIN_ACK` / `LEAVE`, peer table
- [ ] Reliable control requests — request id, retry with backoff, idempotent responses
- [ ] Heartbeat and peer timeout
- [ ] Reconnect without restarting the app
- [ ] `PTT_START` / `PTT_STOP` presence for the UI (not floor control — see below)
- [ ] Optional: BLE rendezvous fallback for networks with AP client isolation or blocked multicast

---

## M4 — Adaptive transport

- [ ] Adaptive jitter target, 20–100 ms, driven by measured jitter and underruns
- [ ] Resize at talkspurt boundaries — free, no audible artefact
- [ ] **Clock drift compensation** — track buffer occupancy trend, correct by micro-resampling
- [ ] Adaptive bitrate with hysteresis (no 32↔24 oscillation)
- [ ] Adaptive FEC driven by measured loss
- [ ] Benchmark adaptive vs fixed across the impairment matrix

---

## M5 — Full duplex

- [ ] Remove the local TX gate; both directions always active
- [ ] N jitter buffers + N decoders, one per live stream
- [ ] Mixer with limiting to prevent clipping
- [ ] Headphones-first (wired for benchmarks — Bluetooth adds 100–200 ms invisible to our telemetry)
- [ ] CPU and battery measurement under sustained duplex

---

## M6 — Voice activation

- [ ] VAD with hangover (800–1500 ms) so speakers aren't cut off between words
- [ ] Talkspurt state machine driving `TALKSPURT_START` / `TALKSPURT_END`
- [ ] DTX during silence

---

## M7 — Echo cancellation

The hardest milestone. Deliberately last.

- [ ] Speaker-reference capture aligned to the microphone stream
- [ ] AEC — build vs adopt decision, reopening ADR 0001's AEC3 note on its own merits
- [ ] Noise suppression, AGC
- [ ] Speakerphone full duplex

---

## M8 — Security

- [ ] Session handshake and key agreement
- [ ] AEAD media encryption (ChaCha20-Poly1305 or AES-GCM)
- [ ] Replay protection — reuses `SeqTracker`'s sliding window with the verdict read as accept/reject
- [ ] Channel auth: open / PIN / invite
- [ ] Benchmark the encryption overhead

---

## M9 — iOS

Deferred: no iOS device available for testing, so the app would be untestable
code. The core keeps cross-compiling throughout so this lands cheaply.

- [ ] `radio_c.h` stable C ABI + Swift wrapper
- [ ] xcframework packaging
- [ ] AVAudioSession / AVAudioEngine capture and playback
- [ ] Local network permission, Bonjour service declaration
- [ ] SwiftUI PTT + diagnostics
- [ ] Conformance test against `protocol/testvectors/`
- [ ] iOS ↔ Android interop

---

## Cross-cutting

- [x] `.gitignore`, including untracked `learn/` teaching notes
- [x] `docs/decisions/` — ADR 0001 (WebRTC)
- [ ] `docs/architecture.md`
- [ ] `radio_c.h` — C ABI for non-C++ consumers
- [ ] Android cross-compile target green (`build-android`, NDK 29 toolchain, arm64-v8a)
- [ ] iOS cross-compile target green (`build-ios`, arm64 device + simulator)
- [ ] GitHub Actions — host build + tests, sanitizer run, both cross-compiles
- [ ] `.clang-format` and a format check
- [ ] Long-run soak test: 10 min / 1 h / 4 h, watching memory, CPU, battery, clock drift, buffer drift

## Optional / stretch

- [ ] Wake-word activation with pre-roll buffer *(deprioritised: ML integration, not a systems problem — competes with M4 and M7 for time)*
- [ ] RTP-compatible mode — buys Wireshark and `ffmpeg` interop; the 16-byte header was shaped to keep this small
- [ ] WebRTC comparison build — latency, loss recovery, CPU, complexity, code size (see ADR 0001 follow-up)
- [ ] Multicast experiment
- [ ] Rust core migration

---

## Deviations from the original roadmap

Full reasoning in the plan file and `docs/decisions/`.

| # | Change | Why |
|---|---|---|
| 1 | Shared C++ core from day one, not the endgame | Android already forces C++; jitter buffer written once, not three times; host benchmarks measure shipping code |
| 2 | 16-byte header, not ~28 | UDP carries its own length; `session_id` belongs to the control plane |
| 3 | Jitter buffer decodes in the consumer (pull) | Playout clock == audio clock; avoids two drifting clocks and a second buffer of latency |
| 4 | Seeded userspace impairment proxy, not `tc`/netem | netem isn't reproducible, so FEC-on vs FEC-off would compare different loss patterns |
| 5 | Host media pipeline before mobile audio | One clock, so E2E latency is exact — the reference for all device numbers |
| 6 | Trace instrumentation from commit one | Retrofitting per-stage timestamps touches every layer |
| 7 | **Floor control deleted** | Distributed arbitration teaches nothing about latency and full duplex discards it; PTT is a local TX gate |
| 8 | One UDP socket, no TCP control plane | One firewall rule, one discovery record; PING/PONG machinery already needed |
| 9 | Interop via test vectors, not by pairing phones | Device pairing confirms interop instead of debugging it |
| 10 | Wake word deprioritised; clock drift promoted | Drift bites in any multi-minute call and is the stronger systems story |
| 11 | iOS compiles from M0, app deferred to M9 | No device to test on |
| 12 | Wi-Fi only; Bluetooth is not a candidate | No datagram semantics, and no way to put an impairment proxy inside the link |

---

## Teaching notes

`learn/` — untracked by design, so a reviewer sees the engineering rather than
the lessons.

| # | Lesson |
|---|---|
| 01 | The C++ you need for this project |
| 02 | Bytes, endianness, and undefined behaviour |
| 03 | Serial numbers and the wrap problem |
| 04 | Designing the packet format |
| 05 | Sequence tracking: loss vs late vs duplicate |
| 06 | Clocks: which one, and why it matters |
| 07 | Measuring jitter, and why averages lie |
