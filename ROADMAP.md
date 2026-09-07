# Roadmap

Nine milestones. Each one is shippable on its own and is validated before the
next begins.

| | Milestone | Status |
|---|---|---|
| [M0](#m0--protocol-transport-and-measurement) | Protocol, transport, measurement, host tooling | **complete** |
| [M1](#m1--media-pipeline-host) | Media pipeline on the host | next |
| [M2](#m2--android) | Android | |
| [M3](#m3--discovery-and-sessions) | Discovery and sessions | |
| [M4](#m4--adaptive-transport) | Adaptive transport | |
| [M5](#m5--full-duplex) | Full duplex | |
| [M6](#m6--voice-activity-detection) | Voice activity detection | |
| [M7](#m7--acoustic-echo-cancellation) | Acoustic echo cancellation | |
| [M8](#m8--media-encryption) | Media encryption | |
| [M9](#m9--ios) | iOS | |

---

## M0 — Protocol, transport and measurement

**Complete.** 159 tests, warning-free, ASan/UBSan clean.

Goal: two endpoints exchanging packets through a reproducible impairment proxy,
with loss, reordering, jitter and round-trip time reported and **validated
against the proxy's own record of what it did**.

- [x] Normative wire-format specification and 35 conformance vectors
- [x] Packet codec with fail-closed validation and classified rejections
- [x] `CLOCK_MONOTONIC_RAW` time base, sample-unit conversions
- [x] Sequence accounting: loss, duplicates, reordering, out-of-window
- [x] RFC 3550 interarrival jitter with talkspurt re-anchoring
- [x] Log-linear latency histogram, 3.1% bounded relative error
- [x] RAII UDP socket, non-blocking, arrival-timestamped at the syscall
- [x] Drift-free departure pacer with bounded catch-up
- [x] NTP-style clock offset with a reported uncertainty bound
- [x] Per-packet, per-stage trace buffer with CSV and JSONL sinks
- [x] `radiobench` — protocol peer: `respond`, `ping`, `send`
- [x] `impair` — seeded impairment proxy with a ground-truth event log
- [x] Adversarial-input coverage and a gated libFuzzer target
- [x] Android arm64 and iOS arm64 cross-compiles building in CI

Acceptance met: reported metrics match the proxy's ground-truth log exactly, and
an identical seed reproduces an identical impairment pattern. Results in
[docs/measurements.md](docs/measurements.md).

**Carried forward:** validation so far is over loopback, which exercises the full
protocol and metrics path but not channel contention, link-layer retries or
power-save behaviour. Real-radio validation folds into M2, where the phone
supplies the second endpoint.

---

## M1 — Media pipeline (host)

Goal: `wav → Opus → UDP → impairment → jitter buffer → decode → wav`, entirely
within one process, so **end-to-end latency is exactly measurable with no
clock-synchronisation error**. That figure becomes the reference every on-device
measurement is compared against.

- [ ] libopus pinned by URL and checksum
- [ ] Opus encoder/decoder wrappers: VOIP mode, 48 kHz mono, 20 ms frames,
      in-band FEC, expected-loss hint, DTX; fully preallocated
- [ ] Lock-free single-producer/single-consumer PCM and packet rings, bounded,
      discarding oldest on overflow
- [ ] Reorder queue with a playout deadline
- [ ] Jitter buffer, **pull model**: decoding happens in the consumer, so the
      playout clock is the audio clock and there is no second clock to drift
      against
- [ ] Packet-loss concealment and FEC-assisted recovery
- [ ] Late-packet accounting, separate from loss
- [ ] `radiobench wavloop` with per-frame stage tracing
- [ ] Unattended benchmark matrix over the impairment scenarios
- [ ] Per-stage latency budget with measured figures

Acceptance: the matrix runs unattended; at 5% burst loss, FEC-on and FEC-off
output generated from an *identical* impairment pattern differ audibly, and the
FEC-recovery counter accounts for the difference.

---

## M2 — Android

Goal: a phone talking to `radiobench` on a workstation, then phone to phone.
Testing against the host peer first puts a debugger, packet capture and full
tracing on one end of every call.

- [ ] Gradle project, Kotlin, Jetpack Compose, `minSdk 26`
- [ ] CMake integration linking the core for arm64-v8a and x86_64
- [ ] Oboe capture and playback in low-latency mode
- [ ] JNI boundary carrying commands and state, never on the audio callback
- [ ] Push-to-talk UI, peer list, connection state
- [ ] Diagnostics screen: RTT, jitter, loss, late, buffer depth, codec timings
- [ ] Conformance vectors executed on device
- [ ] Device audio-latency measurement against the M1 host baseline
- [ ] Real-radio validation carried over from M0

---

## M3 — Discovery and sessions

- [ ] mDNS service discovery (`_fdradio._udp.local`)
- [ ] `JOIN` / `JOIN_ACK` / `LEAVE`, peer table
- [ ] Reliable control requests: request id, backoff, idempotent responses
- [ ] Heartbeat, peer timeout, reconnection without restart
- [ ] Talk-state presence for the UI
- [ ] Optional BLE rendezvous for networks that block multicast or isolate clients

---

## M4 — Adaptive transport

- [ ] Adaptive jitter target driven by measured jitter and underruns, resized at
      talkspurt boundaries where it costs no artefact
- [ ] Clock-drift compensation from buffer-occupancy trend
- [ ] Adaptive bitrate and FEC with hysteresis
- [ ] Adaptive versus fixed, benchmarked across the impairment matrix

---

## M5 — Full duplex

- [ ] Continuous transmit in both directions
- [ ] One jitter buffer and decoder per live stream, mixed with limiting
- [ ] Wired headsets first: Bluetooth adds 100–200 ms that is invisible to our
      own telemetry and would dominate any measurement
- [ ] CPU and battery under sustained duplex

---

## M6 — Voice activity detection

- [ ] Speech detection with hangover, so speakers are not cut off between words
- [ ] Talkspurt state machine driving the wire flags
- [ ] Discontinuous transmission during silence

---

## M7 — Acoustic echo cancellation

The hardest milestone, deliberately last.

- [ ] Speaker reference aligned to the microphone stream
- [ ] Echo cancellation — build versus adopt decided on its own merits
- [ ] Noise suppression and gain control
- [ ] Speakerphone full duplex

---

## M8 — Media encryption

- [ ] Session handshake and key agreement
- [ ] AEAD media encryption
- [ ] Replay protection, reusing the sequence window already in place
- [ ] Channel authentication
- [ ] Encryption overhead benchmarked

---

## M9 — iOS

Deferred until a device is available for testing; the core cross-compiles
throughout so nothing has to be reworked when it lands.

- [ ] Stable C ABI and Swift wrapper, packaged as an xcframework
- [ ] AVAudioSession / AVAudioEngine capture and playback
- [ ] Local-network permission and Bonjour service declaration
- [ ] SwiftUI push-to-talk and diagnostics
- [ ] Conformance vectors on device, then interoperability with Android

---

## Deliberately out of scope

| Not doing | Why |
|---|---|
| NAT traversal / WAN operation | LAN-only scope is what makes a custom transport reasonable at all; see [ADR 0001](docs/decisions/0001-custom-udp-transport-not-webrtc.md) |
| Multicast media | Apple's multicast entitlement requirements add complexity for a bandwidth saving a LAN does not need |
| Wake-word activation | An ML integration task rather than a transport one; competes with M4 and M7 for effort |
| Bluetooth as a transport | No datagram semantics, and an impairment proxy cannot be placed inside the link |
