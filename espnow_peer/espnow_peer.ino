/*
 * espnow_peer.ino - two ESP32 boards talking to each other over ESP-NOW.
 *
 * The same sketch runs on both boards; only NODE_NAME in peer_config.h differs.
 * Each node periodically sends a DATA frame to its partner, the partner replies
 * with an ACK, and the original sender prints the measured round-trip time.
 *
 * Received frames are queued by the ESP-NOW callback and handled from loop(),
 * so nothing slow ever runs inside the Wi-Fi task.
 *
 * Board: ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C3  (Arduino-ESP32 core 2.x or 3.x)
 * Serial monitor: 115200 baud
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "peer_config.h"

/* Older cores do not define these; keep the #if guards below well formed. */
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#ifndef ESP_ARDUINO_VERSION
#define ESP_ARDUINO_VERSION 0
#define ESP_ARDUINO_VERSION_VAL(a, b, c) 0
#endif

/* -------------------------------------------------------------------------
 * Wire format - see docs/PROTOCOL.md
 * ---------------------------------------------------------------------- */
#define PROTO_VERSION 1

enum : uint8_t {
  MSG_HELLO = 0,  /* broadcast "I am here", used for auto discovery */
  MSG_DATA  = 1,  /* payload frame, expects an ACK                  */
  MSG_ACK   = 2   /* acknowledgement, echoes the DATA sequence      */
};

typedef struct __attribute__((packed)) {
  uint8_t  version;      /* PROTO_VERSION                                 */
  uint8_t  type;         /* MSG_HELLO / MSG_DATA / MSG_ACK                */
  uint32_t seq;          /* sequence number of this frame, echoed in ACK  */
  uint32_t uptime_ms;    /* millis() of the sender                        */
  float    value;        /* stand-in for a sensor reading                 */
  char     name[8];      /* NODE_NAME of the sender, NUL padded           */
} espnow_message_t;      /* 22 bytes; ESP-NOW allows up to 250            */

/* One received frame, passed from the Wi-Fi task to loop(). */
typedef struct {
  uint8_t          mac[6];
  espnow_message_t msg;
} rx_item_t;

#define RX_QUEUE_DEPTH 10
#define LED_BLINK_MS   20

/* -------------------------------------------------------------------------
 * State
 *
 * Each counter has exactly one writer: txOk/txFail/rxDropped are written by
 * ESP-NOW callbacks, everything else only by loop().
 * ---------------------------------------------------------------------- */
static const uint8_t BROADCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static QueueHandle_t rxQueue = nullptr;

static uint8_t  peerMac[6];
static bool     peerKnown  = false;
static uint32_t txSeq      = 0;
static uint32_t lastSendMs = 0;
static uint32_t sentAtMs   = 0;   /* timestamp of the DATA frame awaiting an ACK */

static uint32_t dataTx = 0, dataRx = 0, ackRx = 0;
static volatile uint32_t txOk = 0, txFail = 0, rxDropped = 0;
static volatile uint32_t txFailStreak = 0;   /* consecutive radio failures */

#if STATUS_LED_PIN >= 0
static uint32_t ledOffAtMs = 0;
#endif

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static const char *macToStr(const uint8_t *mac) {
  static char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

/* Adds mac to the peer table if it is not there already. A frame can only be
 * sent to a registered peer, including the broadcast address. */
static bool registerPeer(const uint8_t *mac, bool encrypted) {
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = encrypted;
#if ENABLE_ENCRYPTION
  if (encrypted) memcpy(peer.lmk, ESPNOW_LMK, 16);
#endif

  esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK) {
    Serial.printf("[err ] esp_now_add_peer(%s) failed: %s\n",
                  macToStr(mac), esp_err_to_name(err));
    return false;
  }
  return true;
}

/* Registers the sender and, the first time round, keeps it as our partner. */
static void adoptPeer(const uint8_t *mac) {
  if (!registerPeer(mac, ENABLE_ENCRYPTION)) return;
  if (peerKnown) return;

  memcpy(peerMac, mac, 6);
  peerKnown = true;
  txFailStreak = 0;
  Serial.printf("[link] paired with %s\n", macToStr(mac));
}

static void sendMessage(const uint8_t *mac, uint8_t type, uint32_t seq) {
  espnow_message_t msg = {};
  msg.version   = PROTO_VERSION;
  msg.type      = type;
  msg.seq       = seq;
  msg.uptime_ms = millis();
  /* Stand-in for a real sensor: swap this line for your own reading. */
  msg.value     = 20.0f + (float)(esp_random() % 1000) / 100.0f;
  strncpy(msg.name, NODE_NAME, sizeof(msg.name) - 1);

  esp_err_t err = esp_now_send(mac, (const uint8_t *)&msg, sizeof(msg));
  if (err != ESP_OK) Serial.printf("[err ] esp_now_send failed: %s\n", esp_err_to_name(err));
  else if (type == MSG_DATA) dataTx++;
}

static void ledOn() {
#if STATUS_LED_PIN >= 0
  digitalWrite(STATUS_LED_PIN, HIGH);
  ledOffAtMs = millis() + LED_BLINK_MS;
#endif
}

static void serviceLed() {
#if STATUS_LED_PIN >= 0
  if (ledOffAtMs && (int32_t)(millis() - ledOffAtMs) >= 0) {
    digitalWrite(STATUS_LED_PIN, LOW);
    ledOffAtMs = 0;
  }
#endif
}

/* -------------------------------------------------------------------------
 * ESP-NOW callbacks
 *
 * These run in the Wi-Fi task, so they only touch counters and the queue -
 * no Serial, no delay(), no esp_now_send(). The callback signatures changed
 * across Arduino-ESP32 releases, so the wrappers at the bottom adapt them.
 * ---------------------------------------------------------------------- */
static void onSendResult(esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    txOk++;
    txFailStreak = 0;
  } else {
    txFail++;
    txFailStreak++;
  }
}

static void onMessage(const uint8_t *src, const uint8_t *data, int len) {
  if (len != (int)sizeof(espnow_message_t)) return;   /* not one of ours */

  rx_item_t item;
  memcpy(item.mac, src, 6);
  memcpy(&item.msg, data, sizeof(item.msg));
  if (item.msg.version != PROTO_VERSION) return;

  if (!rxQueue || xQueueSend(rxQueue, &item, 0) != pdTRUE) rxDropped++;
}

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 2, 0)
static void sendCb(const wifi_tx_info_t *, esp_now_send_status_t status) {
  onSendResult(status);
}
#else
static void sendCb(const uint8_t *, esp_now_send_status_t status) {
  onSendResult(status);
}
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void recvCb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  onMessage(info->src_addr, data, len);
}
#else
static void recvCb(const uint8_t *mac, const uint8_t *data, int len) {
  onMessage(mac, data, len);
}
#endif

/* -------------------------------------------------------------------------
 * Frame handling, running in loop() context
 * ---------------------------------------------------------------------- */
static void handleFrame(const uint8_t *src, espnow_message_t &msg) {
  msg.name[sizeof(msg.name) - 1] = '\0';   /* the sender may not have terminated it */
  ledOn();

  switch (msg.type) {
    case MSG_HELLO: {
      const bool alreadyPaired = peerKnown;
      adoptPeer(src);
      /* Answer once, and only to a peer we did not already know: replying to
       * every HELLO makes two nodes bounce HELLOs off each other forever. */
      if (!alreadyPaired && peerKnown) sendMessage(src, MSG_HELLO, 0);
      break;
    }

    case MSG_DATA:
      dataRx++;
      adoptPeer(src);
      Serial.printf("[recv] from %s seq=%lu uptime=%lums value=%.2f\n",
                    msg.name, (unsigned long)msg.seq,
                    (unsigned long)msg.uptime_ms, msg.value);
      sendMessage(src, MSG_ACK, msg.seq);
      break;

    case MSG_ACK:
      ackRx++;
      if (msg.seq == txSeq) {
        Serial.printf("[ ack] seq=%lu rtt=%lums\n",
                      (unsigned long)msg.seq,
                      (unsigned long)(millis() - sentAtMs));
      } else {
        Serial.printf("[ ack] seq=%lu (late, no rtt)\n", (unsigned long)msg.seq);
      }
      break;
  }
}

static void drainRxQueue() {
  rx_item_t item;
  while (rxQueue && xQueueReceive(rxQueue, &item, 0) == pdTRUE) {
    handleFrame(item.mac, item.msg);
  }
}

static void serviceLink() {
  /* A partner that stops acknowledging is dropped, and we look for one again. */
  if (peerKnown && txFailStreak >= PEER_LOST_AFTER_FAILURES) {
    Serial.printf("[link] lost %s after %lu failed frames, searching again\n",
                  macToStr(peerMac), (unsigned long)txFailStreak);
    esp_now_del_peer(peerMac);
    peerKnown = false;
    txFailStreak = 0;
  }

  if (millis() - lastSendMs < SEND_INTERVAL_MS) return;
  lastSendMs = millis();

  if (!peerKnown) {
    sendMessage(BROADCAST_ADDR, MSG_HELLO, 0);   /* keep looking for a partner */
    return;
  }

  sentAtMs = millis();
  sendMessage(peerMac, MSG_DATA, ++txSeq);
  Serial.printf("[send] seq=%lu -> %s | data tx %lu rx %lu ack %lu | radio ok %lu fail %lu drop %lu\n",
                (unsigned long)txSeq, macToStr(peerMac),
                (unsigned long)dataTx, (unsigned long)dataRx, (unsigned long)ackRx,
                (unsigned long)txOk, (unsigned long)txFail, (unsigned long)rxDropped);
}

/* -------------------------------------------------------------------------
 * Setup / loop
 * ---------------------------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  delay(300);

#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif

  /* ESP-NOW rides on the station interface; no access point is involved. */
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println();
  Serial.printf("=== ESP-NOW node %s ===\n", NODE_NAME);
  Serial.printf("MAC     : %s\n", WiFi.macAddress().c_str());
  Serial.printf("Channel : %d\n", ESPNOW_CHANNEL);

  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
  if (!rxQueue) {
    Serial.println("[fatal] cannot allocate the receive queue, restarting");
    delay(1000);
    ESP.restart();
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[fatal] esp_now_init() failed, restarting");
    delay(1000);
    ESP.restart();
  }

#if ENABLE_ENCRYPTION
  esp_now_set_pmk((const uint8_t *)ESPNOW_PMK);
  Serial.println("Crypto  : enabled (unicast only)");
#endif

  esp_now_register_send_cb(sendCb);
  esp_now_register_recv_cb(recvCb);

  registerPeer(BROADCAST_ADDR, false);   /* needed before any HELLO can go out */

#if PEER_AUTO_DISCOVER
  Serial.println("Peer    : searching...");
#else
  const uint8_t fixedPeer[6] = PEER_MAC_ADDRESS;
  adoptPeer(fixedPeer);
#endif
}

void loop() {
  drainRxQueue();
  serviceLed();
  serviceLink();
}
