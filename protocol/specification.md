# Full-Duplex Radio Protocol — Version 1

Status: **draft, implemented at M0/M1**
Wire version byte: `1`

A datagram protocol for low-latency voice over a LAN. One UDP socket carries
both media and control. This document is normative: `core/src/proto` implements
it, and `protocol/testvectors/` is the conformance contract that every port
(host, Android/JNI, iOS/Swift) must satisfy.

Keywords MUST / SHOULD / MAY are used per RFC 2119.

---

## 1. Design constraints

The protocol optimises for **timeliness over reliability**. A voice frame that
arrives after its playout deadline is worthless, so nothing in this protocol
retransmits media, and nothing blocks waiting for an earlier frame.

1. **Unreliable datagrams only.** No media retransmission, no ordering
   guarantee, no delivery guarantee. Loss is repaired by codec-level
   concealment and forward error correction, or not at all.
2. **Every packet independently decodable** given only the receiver's stream
   state, so a loss never stalls the pipeline.
3. **Fixed-size headers, no variable-length parsing on the media path.** The
   media header is 16 bytes with no options and no extension mechanism in v1.
4. **Fail closed on anything unrecognised**, so a v2 sender can add semantics
   without a v1 receiver silently misinterpreting them.
5. **Single socket.** Control and media share one 5-tuple: one firewall rule,
   one discovery record, one path for RTT/clock-sync probes, and control
   traffic inherits the media path's telemetry.

### 1.1 Transport parameters

| Parameter | Value |
|---|---|
| Transport | UDP, IPv4 and IPv6 |
| Default port | 47000 (configurable) |
| Byte order | big-endian (network byte order) for all multi-byte fields |
| Max datagram size | 1200 bytes total, header included |
| Media codec | Opus, 48 kHz, mono |
| Default frame duration | 20 ms (960 samples) |

The 1200-byte cap is chosen to survive a 1500-byte path MTU with IPv6 headers
and one layer of tunnelling without fragmentation. A 20 ms / 32 kbps Opus frame
is roughly 80 bytes, so the cap is never approached by voice; it exists to bound
receive buffers and to reject nonsense lengths early.

---

## 2. Common prefix

Every packet of every type begins with the same 4 bytes. A receiver can
therefore classify and version-check any datagram before knowing its type.

```
 offset  size  field
   0      1    version        MUST be 1
   1      1    type           see §5
   2      2    flags          type-specific; reserved bits MUST be 0
```

A datagram shorter than 4 bytes MUST be dropped and counted as malformed.

---

## 3. Media packet (`type = 0x01 AUDIO`)

```
 offset  size  field
   0      1    version        = 1
   1      1    type           = 0x01
   2      2    flags          see §3.1
   4      4    stream_id      identifies one continuous media stream
   8      4    sequence       per-stream packet counter
  12      4    timestamp      per-stream, 48 kHz sample units
  16      n    payload        one Opus packet, n = datagram_len - 16
```

Header is 16 bytes and 4-byte aligned. The payload length is **not** carried in
the header: a UDP datagram already knows its own length, so `n = datagram_len -
16`. A datagram with `type = 0x01` and `datagram_len < 17` (i.e. an empty
payload) MUST be dropped and counted as malformed.

### 3.1 Media flags

| Bit | Mask | Name | Meaning |
|---|---|---|---|
| 0 | `0x0001` | `TALKSPURT_START` | First packet of a stream, or first packet after a DTX gap. Receiver SHOULD reset its playout anchor for this stream. Analogous to the RTP marker bit. |
| 1 | `0x0002` | `TALKSPURT_END` | Last packet the sender intends to send in this talkspurt. Receiver MAY release stream state once the buffer drains. Advisory — it may be lost. |
| 2 | `0x0004` | `FEC` | Sender's encoder had in-band FEC enabled, so this payload probably carries LBRR data for `sequence - 1`. Advisory (see §3.4). |
| 3 | `0x0008` | `DTX` | Payload is a DTX / comfort-noise frame rather than active speech. |
| 4–15 | `0xFFF0` | reserved | MUST be 0 on send. Receiver MUST drop the packet and count it if any is set. |

### 3.2 `stream_id`

A `stream_id` identifies one continuous media stream from one source. It is
chosen uniformly at random over the full 32-bit range when the stream opens, and
a receiver keys stream state on the pair **(source address, `stream_id`)**.

Senders MUST allocate stream ids as follows:

- **PTT and VAD modes:** one new `stream_id` per talkspurt.
- **Full-duplex mode:** one `stream_id` for the whole session, with DTX gaps
  marked by `TALKSPURT_START` on resumption.

Per-talkspurt ids matter for correctness, not tidiness: a straggler from the
previous talkspurt arriving after the next one has begun lands on a stream id
the receiver has already retired, so it is discarded cheaply instead of being
mistaken for a wildly out-of-order packet in the live stream. It also makes
multi-speaker mixing fall out naturally — N live streams means N jitter buffers
and N decoders, with no special case for "same peer talking again".

### 3.3 `sequence` and `timestamp`

`sequence` starts at a uniformly random 32-bit value and increments by exactly 1
for every packet sent on the stream, including DTX frames. It detects loss,
duplication, and reordering.

`timestamp` starts at a uniformly random 32-bit value and increments by the
frame's duration in 48 kHz sample units (960 for a 20 ms frame). It gives the
playout position.

**Both are needed, and they are not redundant.** During a DTX gap the sequence
advances by 1 while the timestamp jumps by the whole silence duration — so
sequence alone cannot tell you where to place audio on the timeline, and
timestamp alone cannot tell you whether something was lost.

Both wrap. All comparisons MUST use serial-number arithmetic modulo 2^32 in the
sense of RFC 1982: `a` precedes `b` iff `0 < (b - a) mod 2^32 < 2^31`. This is
why the initial values may be random anywhere in the range — no implementation
is permitted to assume a low starting point.

### 3.4 On the `FEC` flag being advisory

Opus in-band FEC is embedded by the encoder inside the *next* frame's payload;
whether any given payload actually carries LBRR data is a property of the Opus
bitstream, not of this protocol. The flag is a sender-side hint that lets a
receiver account for FEC in its metrics, and decide policy, without parsing the
Opus payload. A receiver MUST NOT rely on it: it MAY attempt an FEC-assisted
decode on a gap whether or not the flag is set, and MUST tolerate the flag being
set on a payload that turns out to carry no usable LBRR.

---

## 4. Control packets

Control traffic is rare — a few packets per second at most — so it can afford a
wider header than the media path.

```
 offset  size  field
   0      1    version        = 1
   1      1    type           see §5
   2      2    flags          reserved, MUST be 0 in v1
   4      4    request_id     opaque, echoed in the matching response
   8      8    send_time_us   sender's monotonic clock, microseconds
  16      m    body           type-specific, may be empty
```

Control header is 16 bytes. A datagram whose type is a control type and whose
length is less than 16 MUST be dropped and counted as malformed.

`send_time_us` is a **monotonic** clock reading, not a wall clock. It is not
comparable across hosts until clock offset has been estimated (§4.1). Its epoch
is arbitrary and per-process.

### 4.1 `PING` / `PONG` — RTT and clock offset

The four-timestamp NTP exchange. `A` probes, `B` responds.

```
  A                                        B
  │ t1 = A.monotonic()                     │
  │ ── PING  request_id, send_time_us=t1 ─→│ t2 = B.monotonic()
  │                                        │
  │←─ PONG  request_id, send_time_us=t3, ──│ t3 = B.monotonic()
  │         body{ orig_t1, recv_t2 }       │
  │ t4 = A.monotonic()                     │
```

`PING` body is empty. `PONG` body is 16 bytes:

```
 offset  size  field
   0      8    orig_t1        echo of the PING's send_time_us
   8      8    recv_t2        B's clock when the PING arrived
```

with `t3` carried in the `PONG`'s own `send_time_us`. A responder MUST echo
`request_id` and `orig_t1` unmodified; a prober MUST discard a `PONG` whose
`request_id` does not match an outstanding probe.

From one exchange:

```
  rtt    = (t4 - t1) - (t3 - t2)
  offset = ((t2 - t1) + (t3 - t4)) / 2
```

`offset` is B's clock minus A's clock. Both estimates are noisy; §6 describes
how they are filtered. Note that `rtt` subtracts B's processing time, so it is a
network measurement and not a request-latency measurement.

### 4.2 Reliable control requests

Control requests are made reliable by retransmission, not by a transport. A
requester retransmits with the **same** `request_id` on a backoff until it sees
the matching response or gives up. A responder MUST therefore treat requests as
idempotent, deduplicating on `(source address, type, request_id)` over a short
window and re-sending the cached response rather than re-executing.

`PING` needs none of this — an unanswered probe is simply a lost sample, and
retrying it would bias the RTT estimate.

---

## 5. Packet types

| Code | Name | Direction | Layout | Status |
|---|---|---|---|---|
| `0x01` | `AUDIO` | any → any | §3 media | **v1, implemented** |
| `0x02` | `PING` | any → any | §4 control | **v1, implemented** |
| `0x03` | `PONG` | response | §4 control | **v1, implemented** |
| `0x10` | `JOIN` | client → host | §4 control | reserved, M3 |
| `0x11` | `JOIN_ACK` | response | §4 control | reserved, M3 |
| `0x12` | `LEAVE` | any → any | §4 control | reserved, M3 |
| `0x30` | `PTT_START` | any → any | §4 control | reserved, M3 |
| `0x31` | `PTT_STOP` | any → any | §4 control | reserved, M3 |
| `0x40` | `METRICS` | any → any | §4 control | reserved, M3 |

Codes not listed are unassigned. A receiver MUST drop unassigned codes and count
them, exactly as it would an unassigned flag bit.

`PTT_START` / `PTT_STOP` are presence notifications for the UI — "Alice is
talking" — and are explicitly **not** a floor-control grant. There is no floor
arbitration in this protocol: any peer may transmit at any time, and a receiver
mixes whatever streams it has. Push-to-talk is a local gate on the transmit
path, which is what makes push-to-talk and full duplex the same code path with
one boolean difference.

---

## 6. Receiver obligations

### 6.1 Validation order

A receiver MUST reject in this order, counting each rejection class separately,
before touching stream state:

1. `datagram_len < 4` → malformed
2. `version != 1` → bad version
3. `type` unassigned → unknown type
4. any reserved flag bit set → unknown flag
5. length below the minimum for the type (§3, §4) → malformed

Rejection is silent — no error is returned to the sender. Counters exist so the
condition is visible in telemetry rather than in a log nobody reads.

### 6.2 Loss, duplication, reordering

Per stream, using RFC 1982 comparisons on `sequence`, a receiver MUST be able to
report: packets received, packets lost, duplicates, reordered arrivals, and
packets that arrived after their playout deadline ("late"). Late and lost are
distinct and MUST NOT be conflated — a rising late count with flat loss means the
jitter buffer is too shallow, which is a completely different fix from actual
loss.

### 6.3 Interarrival jitter

A receiver SHOULD maintain the RFC 3550 interarrival jitter estimate. With `S`
the packet's `timestamp` and `R` the arrival time in the same 48 kHz units:

```
  D(i-1, i) = (R_i - R_{i-1}) - (S_i - S_{i-1})
  J        += (|D(i-1, i)| - J) / 16
```

`J` is a smoothed estimate in sample units. It is computed from the *difference*
of transit times, so it needs no clock synchronisation — the unknown constant
offset between sender and receiver clocks cancels. This is why jitter is
measurable long before one-way latency is.

### 6.4 Playout

A receiver MUST NOT play a frame after its playout deadline has passed; such a
frame is counted late and discarded. A receiver MUST bound all queues and, on
overflow, discard the **oldest** buffered audio rather than the newest, because
stale voice is worse than a gap.

---

## 7. Versioning

The version byte is checked before anything else, and the failure mode is always
to drop. There is no negotiation in v1 and no extension mechanism on the media
path — a v1 receiver seeing v2 traffic drops all of it and reports a version
mismatch, rather than attempting a partial parse.

The reserved flag bits are the one forward-compatibility affordance, and they
**fail closed**: a receiver drops any packet carrying a flag it does not
understand. Failing open would be worse than useless. If v2 defines a bit
meaning "this payload is encrypted" or "this frame is redundant, do not play it
twice", a v1 receiver that ignored the bit would decode noise or duplicate
audio, and the bug would surface as an audio artefact rather than a counter. The
cost of failing closed is that a v2 sender must not set new bits until it knows
the peer's version; that is a cheap constraint to satisfy in a handshake.

---

## 8. Conformance

`protocol/testvectors/*.json` is the normative contract. Each vector is a hex
datagram plus either its expected decoded fields or the expected rejection
class. Vectors cover valid media and control packets, boundary lengths,
truncation, bad versions, unassigned types, reserved flag bits, and
serial-number wrap.

Any implementation of this protocol — the C++ core, a JNI-side test on Android,
a Swift test on iOS — MUST pass the full vector set. Cross-platform interop is
established by both ends passing the vectors, not by two devices appearing to
work when placed next to each other.
