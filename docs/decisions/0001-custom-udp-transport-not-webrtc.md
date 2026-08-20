# ADR 0001 — Custom UDP transport instead of WebRTC

Status: **accepted**
Date: 2026-08-20

## Context

The system is a low-latency LAN voice intercom: push-to-talk first, then
voice-activated, then full duplex, across Android and iOS. WebRTC is the obvious
off-the-shelf answer and solves most of the problem out of the box, so choosing
not to use it needs justifying rather than assuming.

Two goals carry equal weight here, and they pull in opposite directions:

1. Ship a genuinely good product.
2. Demonstrate real-time transport engineering — protocol design, jitter
   buffering, loss recovery, clock synchronisation, and measured latency.

## Decision

Implement the media transport directly on POSIX UDP sockets with a custom
16-byte packet format (`protocol/specification.md`), our own jitter buffer, and
our own metrics. Do not adopt WebRTC.

## Rationale

### Most of WebRTC's complexity does not apply to a LAN

A large share of WebRTC is NAT traversal and session negotiation for the open
internet — ICE, STUN, TURN, SDP offer/answer. On a single subnet, with mDNS
service discovery, none of it is needed.

This is the load-bearing part of the decision. **If this were a WAN product the
decision would flip**, because hand-writing ICE is a genuinely bad idea and
WebRTC's NAT traversal is worth the entire rest of the dependency. LAN-only
scope is what makes a custom transport reasonable, and if the scope ever widens
to peers across the internet, this ADR should be revisited rather than defended.

### The work WebRTC does for us is the work worth doing

The components that make this project interesting — adaptive jitter buffering,
packet-loss concealment, FEC-versus-PLC comparison, clock drift compensation,
per-stage latency accounting — are precisely what NetEq already implements.
Adopting WebRTC would reduce the project to configuring a library, and the
engineering it is meant to demonstrate would no longer be present in it.

### Instrumentation depth

The latency model this project is built around (`docs/latency-model.md`) needs a
timestamp per packet at every stage: capture, encode, transmit queue, network,
jitter enqueue, jitter dequeue, decode, playout. WebRTC's `getStats()` exposes
aggregate counters over intervals, not per-packet stage traces, because that is
not what it is for.

Extracting those traces would mean patching and maintaining a fork of a very
large codebase. The likely outcome is spending the project's time maintaining a
fork rather than on transport engineering.

### Tuning target mismatch

WebRTC's defaults are conservative because it must work across hostile internet
paths to a huge range of endpoints, so its jitter buffer trades latency for
robustness. This system targets a LAN with 2–10 ms RTT and a 50–70 ms
end-to-end stretch goal. Much of the work would be spent overriding defaults
rather than understanding the tradeoffs behind them.

### Practical cost

A `libwebrtc` checkout runs to tens of gigabytes and requires `depot_tools` and
`gn`; builds take hours. It also imposes its own threading model, clock
abstraction, and logging, which the core would have to be built inside — in
tension with the goal of one small, portable core shared by host benchmarks,
Android, and iOS.

## What WebRTC does better, acknowledged

Recorded honestly, because a decision that only lists the other option's
weaknesses is not a decision:

1. **NetEq is better than what we will build.** Years of production tuning,
   including time-stretching to resize the buffer without audible artefacts.
   Ours will be comprehensible and adequate; theirs is better.
2. **AEC3 is dramatically better than what we will build.** Acoustic echo
   cancellation is the hardest component in the roadmap (M7), and it is the one
   place where adopting an existing implementation is defensible even here. To
   be revisited at M7 on its own merits.
3. **For shipping quickly, WebRTC wins outright.** If the priority became
   delivering to real users within weeks, this decision should be reversed.

## Consequences

- We own everything TCP and WebRTC would have handled: loss detection,
  reordering, pacing, playout deadlines, congestion response. These are exactly
  the decisions that need to be media-aware, so owning them is the point rather
  than the price.
- No NAT traversal. The system works on a LAN, or via a phone hotspot. WAN
  support would require revisiting this ADR.
- No encryption until M8. Interim builds carry unencrypted media, and the packet
  format reserves space for AEAD so that adding it is not a redesign.
- Interoperability with standard tooling is deferred. The 16-byte media header
  is deliberately close to RTP's 12-byte layout so that an RTP-compatible mode
  remains a small change — which would make Wireshark and `ffmpeg` able to
  consume our streams while leaving the jitter buffer and metrics ours.

## Follow-up

A late milestone builds a WebRTC version of the same application and compares
latency, loss recovery, CPU, complexity, and code size against this one. Having
implemented the transport first, that comparison can explain *why* WebRTC is
shaped the way it is, which neither adopting it nor avoiding it would have
taught on its own.
