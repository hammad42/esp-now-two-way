# ESP-NOW Two-Way Link

Two ESP32 boards talking directly to each other over **ESP-NOW** — no Wi-Fi router, no
access point, no IP stack. One sketch is flashed to both boards; they find each other
automatically, exchange data frames every 2 seconds, acknowledge every frame, and print
the round-trip time.

```
   Board A                                   Board B
 +-----------+   1. HELLO (broadcast)      +-----------+
 |  ESP32    | --------------------------> |  ESP32    |
 |  node "A" | <-------------------------- |  node "B" |
 +-----------+   2. HELLO (reply, peers    +-----------+
       |            now know each MAC)           |
       |         3. DATA  seq=n  ------------->  |
       |         4. ACK   seq=n  <-------------  |
```

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
5. Open a serial monitor on each board at **115200 baud**.

Nothing else needs configuring — MAC addresses are discovered at runtime.

Expected output on board A:

```
=== ESP-NOW node A ===
MAC     : 24:6F:28:AE:11:04
Channel : 1
Peer    : searching...
[link] paired with 3C:61:05:12:9B:70
[send] seq=1 -> 3C:61:05:12:9B:70  (tx 1 ok 1 fail 0 | rx 0 ack 0)
[ ack] seq=1 rtt=4ms
[recv] from B seq=1 uptime=6120ms value=24.13
```

## Configuration

Everything tunable lives in [`espnow_peer/peer_config.h`](espnow_peer/peer_config.h):

| Setting | Default | Purpose |
| --- | --- | --- |
| `NODE_NAME` | `"A"` | Label printed in the log — the only per-board change |
| `ESPNOW_CHANNEL` | `1` | Wi-Fi channel; must match on both boards |
| `SEND_INTERVAL_MS` | `2000` | Transmit period |
| `PEER_AUTO_DISCOVER` | `1` | `0` uses the fixed `PEER_MAC_ADDRESS` instead |
| `ENABLE_ENCRYPTION` | `0` | `1` turns on AES-128-CCM for unicast traffic |
| `STATUS_LED_PIN` | `LED_BUILTIN` | Blinks on receive; `-1` disables |

To pin the peer manually, flash
[`tools/get_mac_address`](tools/get_mac_address/get_mac_address.ino) to one board, copy
the printed line into the other board's `PEER_MAC_ADDRESS`, and set
`PEER_AUTO_DISCOVER` to `0` on both.

## How it works

- `WiFi.mode(WIFI_STA)` + `WiFi.disconnect()` — the radio is up but unassociated, which
  is all ESP-NOW needs.
- `esp_wifi_set_channel()` pins both boards to the same channel; a mismatch here is the
  most common reason two nodes never see each other.
- `esp_now_add_peer()` registers the broadcast address first, so discovery HELLOs can go
  out before any peer is known.
- The **send callback** reports whether the radio got a link-layer ACK — that is delivery
  to the peer's radio, not proof the application handled it. The application-level
  `MSG_ACK` reply is what proves an end-to-end round trip, and it is what the RTT figure
  is measured from.
- Callback signatures changed between Arduino-ESP32 2.x, 3.0 and 3.2; the sketch adapts
  with `ESP_ARDUINO_VERSION` guards so it compiles on all three.

Frame layout and message types are documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Project layout

```
espnow_peer/
  espnow_peer.ino     main sketch, flashed to both boards
  peer_config.h       all user-tunable settings
tools/
  get_mac_address/    prints a board MAC, for manual pairing
docs/
  PROTOCOL.md         frame format and message flow
```

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Stuck on `Peer : searching...` | Boards on different `ESPNOW_CHANNEL`, or only one board powered |
| `frame not acknowledged by the radio layer` | Peer out of range, asleep, or reset; the peer MAC is stale |
| `esp_now_add_peer failed: 12` (`ESP_ERR_ESPNOW_FULL`) | More than 20 peers registered |
| Nothing on the serial monitor | Wrong baud rate — it must be 115200 |
| Works, then stops when Wi-Fi is used | Connecting to an AP moves the radio to the AP's channel; put both boards on that channel |
| Encrypted build never pairs | `ENABLE_ENCRYPTION` or the keys differ between boards; keys must be exactly 16 bytes |

## License

MIT — see [LICENSE](LICENSE).
