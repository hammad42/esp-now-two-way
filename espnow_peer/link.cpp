#include "link.h"
#include "led.h"
#include "peer_config.h"
#include "protocol.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

/* Older cores do not define these; keep the #if guards below well formed. */
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#ifndef ESP_ARDUINO_VERSION
#define ESP_ARDUINO_VERSION 0
#define ESP_ARDUINO_VERSION_VAL(a, b, c) 0
#endif

/* -------------------------------------------------------------------------
 * Queued items
 * ---------------------------------------------------------------------- */
typedef struct {
  uint8_t        mac[6];
  int16_t        rssi;
  uint16_t       len;
  espnow_frame_t frame;
} rx_item_t;

typedef struct {
  uint16_t       len;
  espnow_frame_t frame;
} tx_item_t;

/* -------------------------------------------------------------------------
 * State
 *
 * Single-writer rule: radioOk / radioFail / rxQueueDrops and retryNow are
 * written only by the ESP-NOW callbacks, everything else only by loop().
 * ---------------------------------------------------------------------- */
static const uint8_t BROADCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static QueueHandle_t rxQueue = nullptr;
static QueueHandle_t txQueue = nullptr;

static uint8_t  peerMac[6];
static bool     peerKnown   = false;
static uint32_t txSeq       = 0;
static uint32_t lastRxSeq   = 0;   /* last acked seq from the peer, for de-duplication */
static uint32_t lastSendMs  = 0;
static uint32_t sendInterval = SEND_INTERVAL_MS;
static bool     autoSend    = true;
static uint32_t dropStreak  = 0;

/* The single outstanding frame waiting for an ACK (stop and wait). */
static tx_item_t pending;
static bool      pendingActive   = false;
static uint8_t   pendingAttempts = 0;
static uint32_t  pendingSentAt   = 0;
static uint32_t  pendingDeadline = 0;

static volatile bool retryNow = false;   /* set by the send callback on radio failure */

static link_stats_t stats;

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */
const char *msgTypeName(uint8_t type) {
  switch (type) {
    case MSG_HELLO: return "HELLO";
    case MSG_DATA:  return "DATA";
    case MSG_ACK:   return "ACK";
    case MSG_TEXT:  return "TEXT";
    case MSG_CMD:   return "CMD";
    default:        return "?";
  }
}

const char *cmdName(uint8_t cmd) {
  switch (cmd) {
    case CMD_LED_ON:    return "led on";
    case CMD_LED_OFF:   return "led off";
    case CMD_LED_BLINK: return "led blink";
    case CMD_IDENTIFY:  return "identify";
    default:            return "unknown";
  }
}

const char *linkMacToStr(const uint8_t *mac) {
  static char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

/* A frame can only be sent to a registered peer, the broadcast address included. */
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
                  linkMacToStr(mac), esp_err_to_name(err));
    return false;
  }
  return true;
}

static void adoptPeer(const uint8_t *mac) {
  if (!registerPeer(mac, ENABLE_ENCRYPTION)) return;
  if (peerKnown) return;

  memcpy(peerMac, mac, 6);
  peerKnown  = true;
  lastRxSeq  = 0;
  dropStreak = 0;
  Serial.printf("[link] paired with %s\n", linkMacToStr(mac));
  ledBlink(2);
}

/* -------------------------------------------------------------------------
 * Transmit path
 * ---------------------------------------------------------------------- */
static void buildFrame(espnow_frame_t &f, uint8_t type, uint8_t flags, uint32_t seq,
                       const void *payload, uint8_t payloadLen) {
  memset(&f.hdr, 0, sizeof(f.hdr));
  f.hdr.version     = PROTO_VERSION;
  f.hdr.type        = type;
  f.hdr.flags       = flags;
  f.hdr.payload_len = payloadLen;
  f.hdr.seq         = seq;
  f.hdr.uptime_ms   = millis();
  strncpy(f.hdr.name, NODE_NAME, sizeof(f.hdr.name) - 1);
  if (payloadLen && payload) memcpy(f.payload, payload, payloadLen);
}

static bool transmit(const uint8_t *mac, const espnow_frame_t &f, uint16_t len) {
  esp_err_t err = esp_now_send(mac, (const uint8_t *)&f, len);
  if (err != ESP_OK) {
    Serial.printf("[err ] esp_now_send failed: %s\n", esp_err_to_name(err));
    return false;
  }
  stats.txFrames++;
  return true;
}

/* Queues a frame for the peer. Acked types wait their turn in the outbox so
 * only one frame is ever outstanding; that keeps retry bookkeeping trivial. */
static bool enqueue(uint8_t type, const void *payload, uint8_t payloadLen, bool needsAck) {
  if (!peerKnown) return false;

  tx_item_t item;
  buildFrame(item.frame, type, needsAck ? FLAG_NEEDS_ACK : 0,
             needsAck ? ++txSeq : 0, payload, payloadLen);
  item.len = frameLength(item.frame);

  if (xQueueSend(txQueue, &item, 0) != pdTRUE) {
    stats.txQueueDrops++;
    return false;
  }
  return true;
}

/* ACKs jump the queue: they are the reply to a frame we just handled. */
static void sendAckNow(const uint8_t *mac, uint32_t seq) {
  espnow_frame_t f;
  buildFrame(f, MSG_ACK, 0, seq, nullptr, 0);
  transmit(mac, f, frameLength(f));
}

static void serviceTx() {
  if (pendingActive) {
    const bool due = retryNow || (int32_t)(millis() - pendingDeadline) >= 0;
    if (!due) return;
    retryNow = false;

    if (pendingAttempts >= RETRY_MAX) {
      Serial.printf("[drop] %s seq=%lu unacknowledged after %u attempts\n",
                    msgTypeName(pending.frame.hdr.type),
                    (unsigned long)pending.frame.hdr.seq, (unsigned)pendingAttempts);
      stats.dropped++;
      dropStreak++;
      pendingActive = false;
      return;
    }

    pending.frame.hdr.flags |= FLAG_RETRY;
    pendingAttempts++;
    stats.retries++;
    pendingSentAt   = millis();
    pendingDeadline = pendingSentAt + ACK_TIMEOUT_MS;
    transmit(peerMac, pending.frame, pending.len);
    return;
  }

  tx_item_t item;
  if (!peerKnown || xQueueReceive(txQueue, &item, 0) != pdTRUE) return;

  transmit(peerMac, item.frame, item.len);

  if (item.frame.hdr.flags & FLAG_NEEDS_ACK) {
    pending         = item;
    pendingActive   = true;
    pendingAttempts = 1;
    pendingSentAt   = millis();
    pendingDeadline = pendingSentAt + ACK_TIMEOUT_MS;
    retryNow        = false;
  }
}

/* -------------------------------------------------------------------------
 * ESP-NOW callbacks - Wi-Fi task context, so no printing and no sending
 * ---------------------------------------------------------------------- */
static void onSendResult(esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    stats.radioOk++;
  } else {
    stats.radioFail++;
    retryNow = true;   /* no link-layer ACK means no reply is coming; retry at once */
  }
}

static void onMessage(const uint8_t *src, const uint8_t *data, int len, int16_t rssi) {
  if (len < (int)sizeof(espnow_header_t)) return;
  if (len > (int)sizeof(espnow_frame_t))  return;

  rx_item_t item;
  memcpy(item.mac, src, 6);
  item.rssi = rssi;
  item.len  = (uint16_t)len;
  memcpy(&item.frame, data, len);

  if (item.frame.hdr.version != PROTO_VERSION) return;
  if (item.frame.hdr.payload_len != len - sizeof(espnow_header_t)) return;

  if (!rxQueue || xQueueSend(rxQueue, &item, 0) != pdTRUE) stats.rxQueueDrops++;
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
  const int16_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 1;
  onMessage(info->src_addr, data, len, rssi);
}
#else
static void recvCb(const uint8_t *mac, const uint8_t *data, int len) {
  onMessage(mac, data, len, 1);   /* core 2.x does not expose the RSSI here */
}
#endif

/* -------------------------------------------------------------------------
 * Receive path, running in loop() context
 * ---------------------------------------------------------------------- */
static void handleAck(const espnow_header_t &hdr) {
  if (!pendingActive || hdr.seq != pending.frame.hdr.seq) {
    Serial.printf("[ ack] seq=%lu (late, no rtt)\n", (unsigned long)hdr.seq);
    return;
  }

  const uint32_t rtt = millis() - pendingSentAt;
  pendingActive = false;
  dropStreak    = 0;
  stats.acked++;
  stats.rttLast = rtt;
  stats.rttSum += rtt;
  stats.rttCount++;
  if (rtt < stats.rttMin || stats.rttCount == 1) stats.rttMin = rtt;
  if (rtt > stats.rttMax) stats.rttMax = rtt;

  if (stats.lastRssi <= 0) {
    Serial.printf("[ ack] seq=%lu rtt=%lums rssi=%ddBm\n",
                  (unsigned long)hdr.seq, (unsigned long)rtt, (int)stats.lastRssi);
  } else {
    Serial.printf("[ ack] seq=%lu rtt=%lums\n", (unsigned long)hdr.seq, (unsigned long)rtt);
  }
}

static void handleFrame(rx_item_t &item) {
  espnow_header_t &hdr = item.frame.hdr;
  hdr.name[sizeof(hdr.name) - 1] = '\0';   /* the sender may not have terminated it */

  stats.rxFrames++;
  if (item.rssi <= 0) stats.lastRssi = item.rssi;
  ledBlink(1);

  /* Acknowledge before anything else, then ignore a repeat we already acted on:
   * a retransmission usually means our previous ACK was the frame that was lost. */
  if (hdr.flags & FLAG_NEEDS_ACK) {
    sendAckNow(item.mac, hdr.seq);
    if (hdr.seq == lastRxSeq) {
      stats.duplicates++;
      return;
    }
    lastRxSeq = hdr.seq;
  }

  switch (hdr.type) {
    case MSG_HELLO: {
      const bool alreadyPaired = peerKnown;
      adoptPeer(item.mac);
      /* Answer once, and only to a peer we did not already know: replying to
       * every HELLO makes two nodes bounce HELLOs off each other forever. */
      if (!alreadyPaired && peerKnown) {
        espnow_frame_t f;
        buildFrame(f, MSG_HELLO, 0, 0, nullptr, 0);
        transmit(item.mac, f, frameLength(f));
      }
      break;
    }

    case MSG_DATA: {
      if (hdr.payload_len < sizeof(payload_data_t)) break;
      adoptPeer(item.mac);
      payload_data_t d;
      memcpy(&d, item.frame.payload, sizeof(d));
      Serial.printf("[recv] %s seq=%lu uptime=%lums value=%.2f\n",
                    hdr.name, (unsigned long)hdr.seq,
                    (unsigned long)hdr.uptime_ms, d.value);
      break;
    }

    case MSG_TEXT: {
      adoptPeer(item.mac);
      char text[ESPNOW_MAX_PAYLOAD + 1];
      memcpy(text, item.frame.payload, hdr.payload_len);
      text[hdr.payload_len] = '\0';
      Serial.printf("[text] %s: %s\n", hdr.name, text);
      break;
    }

    case MSG_CMD: {
      if (hdr.payload_len < sizeof(payload_cmd_t)) break;
      adoptPeer(item.mac);
      payload_cmd_t c;
      memcpy(&c, item.frame.payload, sizeof(c));
      switch (c.cmd) {
        case CMD_LED_ON:    ledSet(true);  break;
        case CMD_LED_OFF:   ledSet(false); break;
        case CMD_LED_BLINK: ledBlink(c.arg ? c.arg : 3); break;
        case CMD_IDENTIFY:  ledBlink(10); break;
        default: break;
      }
      Serial.printf("[cmd ] %s asked for: %s\n", hdr.name, cmdName(c.cmd));
      break;
    }

    case MSG_ACK:
      handleAck(hdr);
      break;

    default:
      break;
  }
}

static void drainRx() {
  rx_item_t item;
  while (rxQueue && xQueueReceive(rxQueue, &item, 0) == pdTRUE) handleFrame(item);
}

/* -------------------------------------------------------------------------
 * Timers: discovery, the periodic reading, and giving up on a dead peer
 * ---------------------------------------------------------------------- */
static void serviceTimers() {
  if (peerKnown && dropStreak >= PEER_LOST_AFTER_FAILURES) {
    Serial.printf("[link] lost %s after %lu unacknowledged messages, searching again\n",
                  linkMacToStr(peerMac), (unsigned long)dropStreak);
    linkUnpair();
    return;
  }

  if (millis() - lastSendMs < sendInterval) return;
  lastSendMs = millis();

  if (!peerKnown) {
    espnow_frame_t f;
    buildFrame(f, MSG_HELLO, 0, 0, nullptr, 0);
    transmit(BROADCAST_ADDR, f, frameLength(f));
    return;
  }

  if (autoSend) linkSendData(20.0f + (float)(esp_random() % 1000) / 100.0f);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
void linkBegin() {
  memset(&stats, 0, sizeof(stats));
  stats.lastRssi = 1;

  /* ESP-NOW rides on the station interface; no access point is involved. */
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(rx_item_t));
  txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t));
  if (!rxQueue || !txQueue) {
    Serial.println("[fatal] cannot allocate the link queues, restarting");
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
#endif

  esp_now_register_send_cb(sendCb);
  esp_now_register_recv_cb(recvCb);

  registerPeer(BROADCAST_ADDR, false);   /* needed before any HELLO can go out */

#if !PEER_AUTO_DISCOVER
  const uint8_t fixedPeer[6] = PEER_MAC_ADDRESS;
  adoptPeer(fixedPeer);
#endif
}

void linkService() {
  drainRx();
  serviceTx();
  serviceTimers();
}

bool linkSendData(float value) {
  payload_data_t d = { value };
  if (!enqueue(MSG_DATA, &d, sizeof(d), true)) return false;
  Serial.printf("[send] DATA seq=%lu value=%.2f -> %s\n",
                (unsigned long)txSeq, value, linkMacToStr(peerMac));
  return true;
}

bool linkSendText(const char *text) {
  size_t len = strnlen(text, ESPNOW_MAX_PAYLOAD);
  if (len == 0) return false;
  if (!enqueue(MSG_TEXT, text, (uint8_t)len, true)) return false;
  Serial.printf("[send] TEXT seq=%lu -> %s: %s\n",
                (unsigned long)txSeq, linkMacToStr(peerMac), text);
  return true;
}

bool linkSendCommand(uint8_t cmd, uint8_t arg) {
  payload_cmd_t c = { cmd, arg };
  if (!enqueue(MSG_CMD, &c, sizeof(c), true)) return false;
  Serial.printf("[send] CMD seq=%lu -> %s: %s\n",
                (unsigned long)txSeq, linkMacToStr(peerMac), cmdName(cmd));
  return true;
}

bool linkIsPaired()            { return peerKnown; }
const uint8_t *linkPeerMac()   { return peerMac; }
const char *linkOwnMac()       { static String m; m = WiFi.macAddress(); return m.c_str(); }
void linkSetAutoSend(bool on)  { autoSend = on; }
bool linkAutoSend()            { return autoSend; }
uint32_t linkInterval()        { return sendInterval; }
const link_stats_t *linkStats(){ return &stats; }

void linkSetInterval(uint32_t ms) {
  sendInterval = ms < 100 ? 100 : ms;   /* below ~100 ms the log is unreadable */
}

void linkUnpair() {
  if (peerKnown) esp_now_del_peer(peerMac);
  peerKnown     = false;
  pendingActive = false;
  dropStreak    = 0;
  lastRxSeq     = 0;
  xQueueReset(txQueue);
  Serial.println("[link] unpaired, broadcasting HELLO again");
}

void linkResetStats() {
  memset(&stats, 0, sizeof(stats));
  stats.lastRssi = 1;
  Serial.println("[stat] counters cleared");
}

void linkPrintStats() {
  const uint32_t attempted = stats.acked + stats.dropped;
  const uint32_t avg = stats.rttCount ? (uint32_t)(stats.rttSum / stats.rttCount) : 0;

  Serial.println();
  Serial.printf("--- node %s ---------------------------------\n", NODE_NAME);
  Serial.printf("  mac         : %s\n", linkOwnMac());
  Serial.printf("  peer        : %s\n", peerKnown ? linkMacToStr(peerMac) : "none, searching");
  Serial.printf("  channel     : %d\n", ESPNOW_CHANNEL);
  Serial.printf("  auto send   : %s every %lums\n",
                autoSend ? "on" : "off", (unsigned long)sendInterval);
  Serial.printf("  frames      : tx %lu  rx %lu\n",
                (unsigned long)stats.txFrames, (unsigned long)stats.rxFrames);
  Serial.printf("  acked       : %lu of %lu  (%lu retried, %lu given up)\n",
                (unsigned long)stats.acked, (unsigned long)attempted,
                (unsigned long)stats.retries, (unsigned long)stats.dropped);
  if (attempted) {
    Serial.printf("  loss        : %lu%%\n",
                  (unsigned long)((stats.dropped * 100UL) / attempted));
  }
  if (stats.rttCount) {
    Serial.printf("  rtt         : last %lums  min %lums  avg %lums  max %lums\n",
                  (unsigned long)stats.rttLast, (unsigned long)stats.rttMin,
                  (unsigned long)avg, (unsigned long)stats.rttMax);
  }
  if (stats.lastRssi <= 0) Serial.printf("  rssi        : %ddBm\n", (int)stats.lastRssi);
  Serial.printf("  radio       : ok %lu  fail %lu\n",
                (unsigned long)stats.radioOk, (unsigned long)stats.radioFail);
  Serial.printf("  discarded   : %lu duplicates  %lu rx overflow  %lu tx overflow\n",
                (unsigned long)stats.duplicates, (unsigned long)stats.rxQueueDrops,
                (unsigned long)stats.txQueueDrops);
  Serial.println("---------------------------------------------");
  Serial.println();
}
