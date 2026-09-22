# Android app

A Kotlin/Compose app that links the project core directly. The core is consumed
with `add_subdirectory`, not vendored or copied, so the phone runs the *same*
source the host benchmarks in [docs/measurements.md](../docs/measurements.md)
measured. If the Android build needed its own fork of the core, every published
figure would describe different code from the one shipping.

The app captures from the microphone, encodes, sends over UDP, receives,
buffers, decodes and plays — the M1 pipeline with real hardware at both ends —
plus a push-to-talk button, a PING/PONG prober and a diagnostics screen
reporting the core's own counters.

## Build

```bash
cd android
./gradlew :app:assembleDebug
./gradlew :app:installDebug      # with a device attached
adb logcat -s fdradio            # the self-check also goes to logcat
```

## Toolchain

Two things are not the defaults, and both will bite anyone who skips them.

**A JDK.** Gradle needs one, and macOS ships a stub that only tells you to
install Java. Android Studio's bundled runtime works:

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
```

**An external CMake.** The Android SDK bundles CMake 3.22.1. The core requires
3.24, and `FetchContent_Declare(... EXCLUDE_FROM_ALL)` in `cmake/opus.cmake`
requires 3.28, so the SDK's copy cannot configure this project at all. Point the
build at a newer one in `local.properties`, which is machine-local and not
committed:

```properties
sdk.dir=/Users/you/Library/Android/sdk
cmake.dir=/opt/homebrew
```

`cmake.dir` is the prefix containing `bin/cmake`; Gradle also looks for `ninja`
beside it. Installing `cmake;3.31.x` through the SDK manager works equally well
and is the better answer on a machine without Homebrew.

Versions are pinned rather than defaulted: Gradle 8.13 in the wrapper, AGP
8.13.1 and Kotlin 2.1.20 in `gradle/libs.versions.toml`, and
`ndkVersion = "29.0.14206865"` in `app/build.gradle.kts`. AGP 8.13 requires
Gradle 8.13 exactly. The NDK is pinned because leaving it unset makes the libc++
the core is compiled against a function of which AGP release you happen to have.

## Decisions worth knowing

**`minSdk 26.`** AAudio arrives in API 26, and AAudio is the only route to the
low-latency audio this project exists to measure. Below it Oboe falls back to
OpenSL ES, which defeats the purpose of the milestone.

**Two ABIs: `arm64-v8a` and `x86_64`.** 64-bit only. `armeabi-v7a` would double
the native build matrix for devices that cannot usefully run the app, and Play
has required 64-bit since 2019. `x86_64` is for the emulator, which can run the
conformance vectors — but its audio path is not representative, so no latency
figure should ever be quoted from it.

**`ANDROID_STL=c++_static`.** One static core archive linked into one `.so`,
and nothing else in the process shares the C++ runtime, so there is no second
library to ship and keep in step.

**The debug build keeps the native half optimised.** A latency number from an
unoptimised build is meaningless, and that is exactly the number someone will
read off the diagnostics screen.

## The audio path

Oboe 1.9.3, pinned by content hash in `cmake/oboe.cmake` and built from source
as a static library, exactly as libopus is one layer down. The app opens one
input and one output stream and moves PCM between them through the core's
`SpscRing`.

**Two independent streams, not Oboe's `FullDuplexStream`.** The duplex helper
drives input from the output callback and is right when the two must be
sample-locked — echo cancellation, which is M7. It is the wrong shape here: in
the finished product capture goes to the network and playback comes from the
jitter buffer, and the two never meet. The structure is the one that ships; the
microphone-to-speaker wiring is temporary scaffolding so both halves can be
proved before there is a network.

**The capture chain is a measured trade-off, not a preference.** On a Redmi
Note 9 Pro, `VoiceCommunication` — the preset that asks for platform echo
cancellation — gets 960-frame bursts and *no* low-latency mode, while
`VoiceRecognition` and `Unprocessed` get 96-frame bursts, LowLatency and
Exclusive. You cannot have the platform's canceller and its fast capture path at
the same time. The app exposes all three so the comparison can be re-run per
device; see [measurements §9](../docs/measurements.md).

## The JNI boundary

Two kinds of traffic, deliberately going opposite ways:

- **Commands** (`start`, `stop`) go down, from an ordinary thread. They open and
  close streams, which blocks until the audio callback returns.
- **State** comes up by **polling**. The audio callback never calls into Java.

The direction matters more than it looks. The obvious design has the callback
notify the UI when something changes, and that means a JNI call from the audio
thread. A JNI call can block on a class load, on the GC, or on the JNI lock, and
a blocked audio callback is an audible click. There is no safe amount of JNI on
that thread, so the direction is inverted: the callback touches only atomics and
a lock-free ring, and whoever wants to know reads them.

The cost is that the UI learns about an underrun up to one poll interval late.
For a diagnostics screen that is free.

The same rule decides where the *network* measurements are taken. Jitter, loss
and rejection are facts about the path, so they are measured on the network
thread beside the arrival timestamp the kernel handed us — not at playout.
Measuring them at playout would make them conditional on the audio callback
still running, so the counters would freeze at exactly the moment you most want
to read them.

## The control path

Alongside the media there is a probe every 200 ms, and the interval matches
`radiobench ping`'s default on purpose: a figure read off this screen and a
figure printed by the host tool are only comparable if they were sampled on the
same schedule.

**The measurement is the same as the host tool's; only the waiting is
different.** `radiobench ping` sends a probe and then sits in a receive loop
until the PONG arrives or the deadline passes, because measuring is all it is
doing. The network thread here cannot do that — it is also draining the
microphone and servicing media, and blocking for up to a second would stall
both. So probes go out on a schedule, the unanswered ones sit in a small table,
and replies are matched whenever they turn up in the ordinary receive path.

That asynchrony is why the matching is written down carefully in
[`radio::Prober`](../core/include/radio/prober.hpp) and unit-tested with
invented timestamps and no socket. With no blocking read to pair a reply with
its probe, nothing but the request id says which `t1` a given `t4` belongs to —
and pairing the wrong two does not fail loudly. It produces an RTT that is
simply wrong and looks entirely plausible.

**The phone answers PINGs as well as sending them**, through the same
`encode_pong_for()` `radiobench respond` uses. That is what lets the workstation
measure the phone, and it is half of what phone-to-phone needs.

**"Connected" means "answered recently", and nothing else.** UDP has no
connection to ask about, so a socket-level check would happily report a healthy
link to a peer that walked out of range ten seconds ago. The indicator is
defined as a PONG within the last second, which is a claim the code can actually
support.

### What an audio callback may not do

No allocation, no locks, no syscalls, no JNI, no unbounded loops. Every one of
those is easy to write by accident and none fail loudly — they produce an
occasional click indistinguishable from network jitter by the time anyone hears
it. The fixed-size ring, the atomics and the preallocated buffers exist to make
the rule keepable rather than merely stated.
