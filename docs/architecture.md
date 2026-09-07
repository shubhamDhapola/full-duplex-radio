# Architecture

## The central constraint

One C++20 core with no platform dependencies beyond POSIX sockets, consumed by
thin platform layers.

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

The boundary is the point. Host benchmarks measure *the same code* that runs on
the phone, so a latency figure produced on a workstation describes the shipping
implementation rather than a second one that happens to agree. The alternative —
a throwaway host prototype plus separate Kotlin and Swift implementations — means
three copies of the jitter buffer and benchmark numbers that transfer to neither
device.

Android forces C++ regardless, because both Oboe and libopus are C/C++
libraries. Sharing the core therefore costs no additional toolchain.

`core/` includes no platform header. It is handed PCM and hands PCM back.
Sockets are the single exception, and they are wrapped so that no socket header
appears in any public interface — which is what keeps the JNI bridge and the
future Swift-facing C ABI clean.

## Modules

| Module | Responsibility |
|---|---|
| `proto` | Wire format: encode, decode, classify rejections. No state. |
| `net` | `UdpSocket` (owning, non-blocking), `Endpoint`, multi-socket poll, departure pacer |
| `time` | Monotonic time base, sample-unit conversion, clock-offset estimation |
| `metrics` | Sequence accounting, interarrival jitter, latency histogram, per-stage tracing |
| `sim` | Reproducible impairment model, used by the proxy and by tests |
| `jitter` | Reorder queue and jitter buffer *(M1)* |
| `audio` | Opus wrappers, lock-free rings, packet pool *(M1)* |
| `session` | Peer table, reliable control requests, talkspurt state *(M3)* |

## Data path

```
  microphone ─→ frame accumulation ─→ Opus encode ─→ pacer ─→ UDP
                                                                │
                                                             network
                                                                │
  speaker ←── Opus decode ←── jitter buffer ←── reorder queue ←─┘
```

Nine trace points along that path — one per stage — are recorded per packet and
keyed on `(stream_id, sequence)`. Sender-side and receiver-side stamps carry
different clocks and are joined offline, so the timestamps are deliberately
*not* carried in the packet: four 64-bit fields would grow an 80-byte frame by
40%, changing the airtime, queueing and loss being measured.

## Threading (M1 onward)

```
  audio input thread          network thread
        │                           │
        ▼                           ▼
   PCM ring (SPSC)            reorder queue
        │                           │
        ▼                           ▼
   encoder ─→ packet ring ─→ UDP TX / RX ─→ jitter buffer
                                                  │
                                       audio output callback
                                       (decodes here — pull model)
```

Two rules govern everything on these paths:

**No allocation.** Every buffer is a fixed-size member or comes from a pool
allocated at startup. `malloc` takes a lock and may call into the kernel; an
audio callback has a hard 20 ms deadline, and late is indistinguishable from
wrong.

**No blocking.** No mutex, no file I/O, no name resolution. `Endpoint::parse`
refuses hostnames outright rather than documenting that `getaddrinfo` must not
be called from the media path — making the function incapable of blocking is
stronger than agreeing not to call it.

Lock-free structures are used only where a real-time thread meets a
non-real-time one. Session state, which no audio callback touches, uses ordinary
synchronisation.

## Decisions worth knowing about

Full records in [decisions/](decisions/).

**UDP, with no retransmission of media.** TCP delivers in order, so one lost
segment blocks everything behind it until the retransmit completes — a single
loss damages four frames instead of one and adds a round trip to every frame
after it. In real-time media a late frame is worth no more than a missing one.

**The jitter buffer decodes in the consumer, not the producer.** Decoding on the
network thread into a PCM ring creates two independent clocks — a timer deciding
playout and the audio device consuming — which drift against each other, plus a
second buffer of latency. Pulling makes the playout clock *be* the audio clock.

**One socket for media and control.** One firewall rule, one discovery record,
one path for RTT probes, and control traffic inherits the media path's
telemetry. Control reliability is a small request/ack/retry layer rather than a
second transport.

**No floor control.** Push-to-talk is a local gate on the transmit path plus a
talkspurt marker on the wire, not a distributed grant. Multiple simultaneous
speakers is then the full-duplex receive path, which is needed anyway — so
push-to-talk becomes a subset of full duplex rather than something full duplex
has to dismantle.

**Fail closed on anything unrecognised.** Unknown versions, unassigned types and
reserved flag bits are dropped and counted. A receiver that ignored an
unrecognised flag could feed ciphertext to the decoder or play audio twice, and
the failure would present as an artefact a user hears rather than a counter an
engineer can read.

**Reproducible impairment rather than `tc netem`.** netem cannot replay a loss
pattern, so any A/B comparison includes an unknown amount of pattern noise. The
model is seeded and its random transforms are hand-written, because the standard
library's distributions are not portable across implementations.

## Portability

The core cross-compiles for Android arm64 and iOS arm64 in CI from M0, long
before either app exists, so portability cannot rot silently. The host build
additionally runs on Linux and macOS.

Platform-specific work stays outside `core/`: audio capture and playback,
permissions, service discovery, audio routing, background execution, and UI.
