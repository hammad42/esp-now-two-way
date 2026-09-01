# ESP-NOW Two-Way Link

Two ESP32 boards talking directly to each other over **ESP-NOW** — no Wi-Fi router, no
access point, no IP stack. One sketch is flashed to both boards; they find each other
automatically, exchange acknowledged messages with automatic retransmission, and take
typed commands from the serial monitor so you can chat between boards, drive the other
board's LED, and watch the link quality.

```
   Board A                                   Board B
 +-----------+   1. HELLO (broadcast)      +-----------+
 |  ESP32    | --------------------------> |  ESP32    |
 |  node "A" | <-------------------------- |  node "B" |
 +-----------+   2. HELLO (reply, both     +-----------+
       |            now know the MAC)            |
       |         3. DATA / TEXT / CMD  seq=n -->  |
       |         4. ACK seq=n  <----------------  |
```

## Features

- **Automatic pairing** — boards discover each other by broadcast, no MAC addresses to
  hardcode
- **Reliable messages** — every message is acknowledged, retransmitted on timeout, and
  de-duplicated at the receiver
- **Round-trip timing and RSSI** — last/min/avg/max RTT, loss percentage, signal strength
- **Serial console** — chat lines, remote LED control, live tuning of the link
- **Variable-length protocol** — 20-byte header plus up to 200 bytes of payload, sent as
  four message types
- **Optional AES-128-CCM encryption** — one setting, keys in `peer_config.h`
- Runs on Arduino-ESP32 core **2.x and 3.x**, on ESP32 / S2 / S3 / C3

## What is ESP-NOW

ESP-NOW is Espressif's connectionless link-layer protocol. Frames are sent inside Wi-Fi
action frames addressed by MAC, so there is no association, no DHCP and no handshake —
a packet leaves the radio in a couple of milliseconds. Practical limits:

| Property | Value |
| --- | --- |
| Payload | 250 bytes per frame |
| Peers | 20 (up to 6 encrypted) |
| Range | ~50 m indoors, 100–200 m line of sight |
| Latency | typically 2–10 ms round trip |
| Security | optional AES-128-CCM per peer |

## Hardware

- 2 × ESP32 boards (ESP32, S2, S3 or C3 — they may be different models)
- 2 × USB data cables
- Two serial monitors, so you can watch both sides of the conversation

## Quick start

1. Install the **ESP32 board package** in Arduino IDE (Boards Manager → "esp32" by
   Espressif). Core 2.x and 3.x are both supported.
2. Open [`espnow_peer/espnow_peer.ino`](espnow_peer/espnow_peer.ino).
3. Flash **board 1** as is (`NODE_NAME "A"` in
   [`peer_config.h`](espnow_peer/peer_config.h)).
4. Change that one line to `#define NODE_NAME "B"` and flash **board 2**.
5. Open a serial monitor on each board at **115200 baud**, line ending **Newline**.

Nothing else needs configuring — MAC addresses are discovered at runtime.

Expected output on board A:

```
=== ESP-NOW node A, protocol v2 ===
mac     : 24:6F:28:AE:11:04
channel : 1
peer    : searching...
[link] paired with 3C:61:05:12:9B:70
[send] DATA seq=1 value=24.31 -> 3C:61:05:12:9B:70
[ ack] seq=1 rtt=4ms rssi=-41dBm
[recv] B seq=1 uptime=6120ms value=22.87
```

## Serial console

Type into the serial monitor and press enter. `help` lists everything.

| Command | Effect |
| --- | --- |
| `send <text>` | Send a chat line to the other board (up to 200 bytes) |
| `led on` / `led off` | Switch the **other** board's LED |
| `led blink [n]` | Blink the other board's LED n times (default 3) |
| `identify` | Long blink burst on the other board — which one is which |
| `ping` | Send one reading now and time the round trip |
| `auto on` / `auto off` | Start or stop the periodic reading |
| `interval <ms>` | Change the periodic reading interval |
| `peer` | Show pairing state and both MAC addresses |
| `unpair` | Forget the peer and run discovery again |
| `stats` | Counters, loss percentage, RTT min/avg/max, RSSI |
| `stats reset` | Clear the counters |

A short session, typed on board A:

```
> send hello from the workbench
[send] TEXT seq=12 -> 3C:61:05:12:9B:70: hello from the workbench
[ ack] seq=12 rtt=5ms rssi=-43dBm
> led blink 5
[send] CMD seq=13 -> 3C:61:05:12:9B:70: led blink
> stats
--- node A ---------------------------------
  mac         : 24:6F:28:AE:11:04
  peer        : 3C:61:05:12:9B:70
  channel     : 1
  auto send   : on every 2000ms
  frames      : tx 41  rx 39
  acked       : 20 of 20  (1 retried, 0 given up)
  loss        : 0%
  rtt         : last 5ms  min 3ms  avg 5ms  max 14ms
  rssi        : -43dBm
  radio       : ok 41  fail 0
  discarded   : 0 duplicates  0 rx overflow  0 tx overflow
---------------------------------------------
```

Meanwhile board B prints `[text] A: hello from the workbench` and blinks.

## Configuration

Everything tunable lives in [`espnow_peer/peer_config.h`](espnow_peer/peer_config.h):

| Setting | Default | Purpose |
| --- | --- | --- |
| `NODE_NAME` | `"A"` | Label printed in the log — the only per-board change |
| `ESPNOW_CHANNEL` | `1` | Wi-Fi channel; must match on both boards |
| `SEND_INTERVAL_MS` | `2000` | Periodic reading interval |
| `ACK_TIMEOUT_MS` | `300` | How long to wait for an ACK before retransmitting |
| `RETRY_MAX` | `3` | Total attempts per message, first one included |
| `RX_QUEUE_DEPTH` / `TX_QUEUE_DEPTH` | `8` / `4` | Buffered frames in and out |
| `PEER_AUTO_DISCOVER` | `1` | `0` uses the fixed `PEER_MAC_ADDRESS` instead |
| `PEER_LOST_AFTER_FAILURES` | `3` | Messages given up on in a row before re-running discovery |
| `ENABLE_ENCRYPTION` | `0` | `1` turns on AES-128-CCM for unicast traffic |
| `STATUS_LED_PIN` | `2` | Blinks on receive; `-1` disables |

To pin the peer manually, flash
[`tools/get_mac_address`](tools/get_mac_address/get_mac_address.ino) to one board, copy
the printed line into the other board's `PEER_MAC_ADDRESS`, and set
`PEER_AUTO_DISCOVER` to `0` on both.

## How it works

- `WiFi.mode(WIFI_STA)` + `WiFi.disconnect()` — the radio is up but unassociated, which
  is all ESP-NOW needs. `esp_wifi_set_channel()` pins both boards to the same channel; a
  mismatch here is the most common reason two nodes never see each other.
- The receive callback runs **in the Wi-Fi task**, so it only copies the frame into a
  FreeRTOS queue and returns. All the slow work — printing, replying, blinking — happens
  in `loop()`. Sending or blocking inside that callback stalls the Wi-Fi stack.
- A HELLO is answered **once, and only by a node that did not already know the sender**.
  Replying to every HELLO makes two nodes bounce broadcasts off each other indefinitely.
- **One message is outstanding at a time.** It is retransmitted every `ACK_TIMEOUT_MS`
  until an ACK arrives or `RETRY_MAX` attempts are used; later messages wait in the
  outbox. A radio-level failure retries immediately instead of waiting out the timeout,
  since no reply can be coming.
- The receiver **acknowledges first, then de-duplicates** on the sequence number: a
  retransmission normally means our previous ACK was what got lost, so it must be
  answered again but not acted on twice.
- After `PEER_LOST_AFTER_FAILURES` messages are given up on, the peer is dropped and the
  node returns to broadcasting HELLO, so a partner that reboots is picked up again.
- Callback signatures changed between Arduino-ESP32 2.x, 3.0 and 3.2; the sketch adapts
  with `ESP_ARDUINO_VERSION` guards so it compiles on all three. RSSI is only available
  from the 3.x receive info, and is reported as unavailable on 2.x.

Frame layout and message types are documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Project layout

```
espnow_peer/
  espnow_peer.ino     setup() and loop(), wiring the modules together
  peer_config.h       all user-tunable settings
  protocol.h          the bytes on the air: header, types, payloads
  link.h / link.cpp   pairing, retries, de-duplication, statistics
  console.h/.cpp      the serial command interface
  led.h / led.cpp     non-blocking status LED
tools/
  get_mac_address/    prints a board MAC, for manual pairing
docs/
  PROTOCOL.md         frame format, message flow, reliability rules
```

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Stuck on `peer : searching...` | Boards on different `ESPNOW_CHANNEL`, or only one board powered |
| `[drop] ... unacknowledged after 3 attempts` | Peer out of range, asleep or reset — the node re-runs discovery on its own |
| `esp_now_add_peer failed: ESP_ERR_ESPNOW_FULL` | Peer table full: 20 peers, of which 6 encrypted |
| `rx overflow` climbing in `stats` | Frames arriving faster than `loop()` drains them; raise `RX_QUEUE_DEPTH` |
| `outbox full, try again shortly` | More messages queued than `TX_QUEUE_DEPTH`, while one waits for its ACK |
| Console commands do nothing | Serial monitor line ending is not set to **Newline** |
| Nothing on the serial monitor | Wrong baud rate — it must be 115200 |
| Works, then stops when Wi-Fi is used | Connecting to an AP moves the radio to the AP's channel; put both boards on that channel |
| Encrypted build never pairs | `ENABLE_ENCRYPTION` or the keys differ between boards; keys must be exactly 16 bytes |

## License

MIT — see [LICENSE](LICENSE).
