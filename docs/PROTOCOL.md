# Wire protocol

Version 2. A fixed 20-byte header followed by 0–200 bytes of payload whose meaning
depends on the message type. Only `sizeof(header) + payload_len` bytes are transmitted,
so a HELLO costs 20 bytes on the air and a full chat line costs 220 — comfortably inside
the 250-byte ESP-NOW limit.

Both boards run the same code, so there is no client and no server: either side may speak
first, and both keep their own sequence numbers.

## Header

`espnow_header_t`, `__attribute__((packed))`, little-endian (both ends are Xtensa or
RISC-V, so no byte swapping is needed):

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `version` | `uint8_t` | `2`. Frames with another version are dropped |
| 1 | `type` | `uint8_t` | `0` HELLO, `1` DATA, `2` ACK, `3` TEXT, `4` CMD |
| 2 | `flags` | `uint8_t` | `0x01` needs ACK, `0x02` is a retransmission |
| 3 | `payload_len` | `uint8_t` | Payload bytes that follow, 0–200 |
| 4 | `seq` | `uint32_t` | Per-sender counter; an ACK echoes the number it answers |
| 8 | `uptime_ms` | `uint32_t` | Sender `millis()` at transmit time |
| 12 | `name` | `char[8]` | Sender `NODE_NAME`, NUL padded |

A receiver drops anything shorter than the header, longer than a full frame, or whose
`payload_len` disagrees with the actual length. That keeps unrelated ESP-NOW traffic on
the same channel out of the way, and means the payload length can be trusted afterwards.

## Message types

| Type | Payload | Acked | Purpose |
| --- | --- | --- | --- |
| `HELLO` | none | no | Discovery. Broadcast while unpaired, then answered unicast |
| `DATA` | `float value` | yes | The periodic reading |
| `ACK` | none | no | Answers one acked frame; `seq` echoes it |
| `TEXT` | UTF-8 bytes, not terminated | yes | A chat line typed at the console |
| `CMD` | `uint8_t cmd, uint8_t arg` | yes | Remote LED control: on, off, blink ×`arg`, identify |

`TEXT` payloads are not NUL-terminated on the air; the receiver terminates them using
`payload_len` before printing.

## Discovery

While no peer is known, a node broadcasts a HELLO once per interval. A node that receives
one registers the sender and replies with a **unicast** HELLO **only if it was not
already paired**. That condition is what terminates the exchange: an unconditional reply
leaves the two nodes answering each other's HELLOs forever.

```
A                                   B
|--- HELLO (broadcast) ------------>|   B registers A, was unpaired -> replies
|<-- HELLO (unicast) ---------------|   A registers B, was unpaired -> replies
|--- HELLO (unicast) -------------->|   B is already paired -> silent, exchange ends
|                                   |
|--- DATA seq=1 ------------------->|
|<-- ACK  seq=1 --------------------|   A prints the round-trip time
|                                   |
|<-- DATA seq=1 --------------------|   B runs the same loop, its own sequence
|--- ACK  seq=1 ------------------->|
```

A node that is already paired stays silent when a rebooted partner broadcasts HELLO — and
the rebooted node re-pairs anyway from the next DATA frame it receives, because DATA also
registers its sender.

Both nodes transmit on their own timer, so the two directions are independent: the link is
genuinely symmetric rather than request/response.

## Reliability

Messages carrying `FLAG_NEEDS_ACK` are sent **one at a time**. The sender keeps the frame
until the matching ACK arrives; if `ACK_TIMEOUT_MS` passes it sets `FLAG_RETRY` and sends
it again, up to `RETRY_MAX` attempts in total, then gives the message up and counts it as
lost. Messages queued in the meantime wait their turn in the outbox, which is what the
`outbox full` warning refers to.

A link-layer failure short-circuits the timeout: when the radio reports that a frame was
not acknowledged at the 802.11 layer, no application reply can be coming, so the retry
happens immediately rather than `ACK_TIMEOUT_MS` later.

Two separate notions of "delivered" are therefore visible in `stats`:

1. **Radio ok/fail** — from the ESP-NOW send callback. The peer's radio received the
   frame. It is not meaningful for broadcasts, which always report success.
2. **Acked** — the `MSG_ACK` frame came back. This is the only evidence that the peer's
   firmware parsed the message, and it is what the RTT is measured from.

The receiver **acknowledges before de-duplicating**. A retransmission normally means the
original ACK was the frame that got lost, so it must be answered again — but it is then
discarded rather than acted on twice, by comparing `seq` with the last acked sequence
from that peer. Re-running a `led on` command or printing a chat line twice would
otherwise be visible to the user.

Sequence numbers are per sender and only assigned to acked messages, so an ACK's echoed
`seq` never disturbs the de-duplication state. They wrap after 2³² messages, which at the
default interval is about 270 years.

The round-trip time is measured from the **last attempt**, not the first, and only when
the ACK matches the outstanding sequence number; a late ACK arriving after the sender has
moved on is logged without a timing figure.

## Extending it

- Bigger payloads: up to 200 bytes fit today, and the cap is just `ESPNOW_MAX_PAYLOAD`
  against the 250-byte ESP-NOW frame limit. Add fields to a payload struct and bump
  `PROTO_VERSION` — mismatched versions are dropped rather than misparsed.
- A new message type: add it to the `MSG_*` enum, give it a payload struct, and handle it
  in `handleFrame()`. Set `FLAG_NEEDS_ACK` if losing it would matter.
- More than two nodes: `peerKnown` holds a single partner today. Replace it with an array
  (ESP-NOW allows 20 peers) and give each peer its own `lastRxSeq` and outstanding frame.
  The HELLO exchange already works many-to-many, since HELLOs are broadcast.
- Encryption: set `ENABLE_ENCRYPTION` to `1` and change the PMK/LMK in `peer_config.h`.
  The PMK is shared by the whole network, the LMK is per peer, both are 16 bytes, and at
  most 6 peers may be encrypted. Broadcast HELLOs stay in plaintext by design.
