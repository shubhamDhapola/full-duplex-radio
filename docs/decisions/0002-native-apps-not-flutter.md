# ADR 0002 — Native Android and iOS apps rather than Flutter

Status: **accepted**
Date: 2026-09-07

## Context

The project targets Android and iOS. A cross-platform UI toolkit — Flutter, or
React Native — would let the application layer be written once, and the obvious
question is why it is not being used.

The question is reasonable enough that it deserves an accounting rather than a
preference, particularly since the shared C++ core already removes the
duplication people usually reach for Flutter to avoid.

## Decision

Write the application layer natively: Kotlin with Jetpack Compose on Android,
Swift with SwiftUI on iOS. Keep the protocol, jitter buffer, codec and metrics in
the shared C++ core, consumed through JNI and a C ABI respectively.

## Rationale

### The duplication Flutter removes is already gone

| Concern | Shared today? | Would Flutter help? |
|---|---|---|
| Protocol, jitter buffer, codec, metrics | Yes — C++ core | No, already shared |
| UI: talk button, peer list, diagnostics | No | **Yes** |
| Permissions flow | No | Mostly |
| Audio capture and playback | No | No |
| Audio routing: speaker, headset, Bluetooth | No | No |
| `AVAudioSession` configuration | No | No — Objective-C/Swift only |
| Background execution | No | No |
| mDNS service discovery | No | Partly; plugin quality varies |

Flutter's saving is the UI and some plumbing. In this application the UI is a
push-to-talk button, a peer list and a statistics table — days of work per
platform, not months. The audio path, which is the substance of the product, is
not shared by adopting Flutter.

### It adds a boundary in the worst possible place

A real-time audio callback must run on a high-priority operating-system thread,
must not allocate, must not block, and must complete well inside 20 ms. Dart
cannot satisfy that, and Flutter's platform channels are asynchronous, passing
through the event loop.

So the audio engine still lives in Kotlin/Oboe and Swift/AVAudioEngine, and the
result is an additional layer rather than one fewer:

```
  Dart / Flutter UI
        |  platform channel (async) or dart:ffi
  Kotlin shim              Swift shim          <- still required
        |                       |
  Oboe + C++ core        AVAudioEngine + C++ core
```

"Kotlin UI plus Swift UI" becomes "Dart UI plus Kotlin shim plus Swift shim".

### The strongest form of the counter-argument, and why it still loses

There is a better version of the Flutter proposal than the one usually made, and
it deserves stating fairly: `dart:ffi` calls C synchronously, Oboe is a C++
library, and iOS AudioToolbox/AudioUnit is a C API. Audio could therefore be
pushed entirely into C++ on both platforms, leaving Dart purely for UI. Dart's
garbage collector pauses Dart isolates rather than native threads, so it would
not stall an audio thread.

That architecture is coherent. It is rejected because:

- `AVAudioSession` — category, mode, route, interruption handling — is
  Objective-C/Swift only and must be configured before audio works at all.
- It replaces `AVAudioEngine` with hand-written AudioUnit code, which is *more*
  platform-specific work, not less.
- Flutter's engine runs its own threads, which compete for CPU on a device where
  the audio deadline is hard.

### The measurement argument

The project's central deliverable is a per-stage latency budget: capture, frame
accumulation, encode, transmit queue, network, jitter buffer, decode, playback.
Producing that requires being inside the platform audio stack — Oboe's buffer
sizing, AAudio's low-latency path, the callback's actual period.

A finding such as "the device's audio buffer contributes more latency than the
network" is only reachable from there. Behind a platform channel it is not
observable, and it is one of the more interesting results this project can
produce.

### The practical timing

There is no iOS device available for testing, so iOS is milestone 9 — the last
one, deferred until hardware exists.

Adopting Flutter now would pay an architectural cost immediately, in the layer
where the difficult bugs live, in exchange for a saving that arrives at
milestone 9 and may never arrive at all. Native Android first keeps the cost
where the value is.

## Consequences

- The UI is written twice. Accepted: it is small, and the two toolkits are
  pleasant enough that the second is faster than the first.
- Two platform audio integrations, Oboe and AVAudioEngine. This is not a cost
  of the decision — it would be required under Flutter too.
- Diagnostics and instrumentation are written twice. Mitigated by keeping every
  counter and histogram in the core, so the platform layer only renders values
  it is handed.
- Hot reload is unavailable for UI iteration, which is a genuine convenience
  loss on the diagnostics screen.

## When to revisit

- **If the product grows a substantial UI.** If screens, navigation and forms
  come to dominate the work, the balance changes and this decision should be
  reversed for the application layer.
- **If both devices are in hand and shipping speed becomes the priority.**
- **If the goal changes.** The project exists partly to build low-latency
  engineering depth, and the platform audio work is where that lives. If that
  ceased to be a goal, this decision loses its main support.

Reversal is not expensive, which is worth noting: the C++ core is indifferent to
what calls it, so an application layer could be replaced without touching the
transport, the jitter buffer or the metrics. That is a deliberate property of
the boundary described in `docs/architecture.md`, not a coincidence.
