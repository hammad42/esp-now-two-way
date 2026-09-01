# Wire protocol

A small fixed-size struct sent as the ESP-NOW payload. Both boards run the same code, so
there is no client and no server — either side may speak first.

## Frame

`espnow_message_t`, 22 bytes, `__attribute__((packed))`, little-endian (both ends are
Xtensa/RISC-V, so no byte swapping is needed):

| Offset | Field | Type | Meaning |
| --- | --- | --- | --- |
| 0 | `version` | `uint8_t` | Protocol version, currently `1`. Frames with another version are dropped |
| 1 | `type` | `uint8_t` | `0` HELLO, `1` DATA, `2` ACK |
| 2 | `seq` | `uint32_t` | Sequence number of a DATA frame; an ACK echoes the number it answers |
| 6 | `uptime_ms` | `uint32_t` | Sender `millis()` at transmit time |
| 10 | `value` | `float` | Application payload — replace with your own reading |
| 14 | `name` | `char[8]` | Sender `NODE_NAME`, NUL padded |

The receiver drops anything whose length is not exactly `sizeof(espnow_message_t)`, which
keeps unrelated ESP-NOW traffic on the same channel out of the way.

## Message types

**HELLO (0)** — sent to `FF:FF:FF:FF:FF:FF` while no peer is known, once per
`SEND_INTERVAL_MS`. A node that receives a HELLO registers the sender as its peer and
replies with its own HELLO, so both sides are paired after one exchange. `seq` is unused.

**DATA (1)** — unicast to the paired peer, `seq` incrementing from 1. Carries `value`
and `uptime_ms`.

**ACK (2)** — unicast reply to a DATA frame, echoing its `seq`. The originator subtracts
its send timestamp from `millis()` on arrival to get the round-trip time.

## Sequence

```
A                                   B
|--- HELLO (broadcast) ------------>|   B registers A
|<-- HELLO (broadcast) -------------|   A registers B
|                                   |
|--- DATA seq=1 ------------------->|
|<-- ACK  seq=1 --------------------|   A prints rtt
|                                   |
|<-- DATA seq=1 --------------------|   B runs the same loop
|--- ACK  seq=1 ------------------->|
```

Both nodes transmit on their own timer, so the two directions are independent — the link
is genuinely symmetric rather than request/response.

## Delivery guarantees

There are two separate notions of "delivered":

1. **Radio ACK** — reported by the ESP-NOW send callback. It means the peer's radio
   received the frame at the 802.11 layer. It is not reported for broadcast frames, which
   always come back as success.
2. **Application ACK** — the `MSG_ACK` frame. It is the only evidence that the peer's
   firmware parsed the message.

Neither is retried. A lost DATA frame simply leaves a gap in the sequence numbers, which
is visible in the log. If your application needs reliability, retransmit on a missing ACK
and de-duplicate on `seq` at the receiver.

## Extending it

- Raise the payload: anything up to 250 bytes fits in one frame. Add fields to the end of
  the struct and bump `PROTO_VERSION`.
- More than two nodes: `peerKnown` holds a single peer today. Replace it with an array
  (ESP-NOW allows 20 peers) and keep the HELLO exchange as-is — it already works
  many-to-many, since HELLOs are broadcast.
- Encryption: set `ENABLE_ENCRYPTION` to `1` and change the PMK/LMK in `peer_config.h`.
  The PMK is shared by the whole network, the LMK is per peer, both are 16 bytes, and at
  most 6 peers may be encrypted. Broadcast HELLOs stay in plaintext by design.
