/*
 * espnow_peer.ino - two ESP32 boards talking to each other over ESP-NOW.
 *
 * The same sketch runs on both boards; only NODE_NAME in peer_config.h differs.
 * The boards discover each other, exchange acknowledged messages with automatic
 * retransmission, and accept typed commands from the serial monitor: chat lines,
 * remote LED control and link statistics.
 *
 *   protocol.h   the bytes on the air
 *   link.cpp     pairing, retries, statistics
 *   console.cpp  the serial command interface
 *   led.cpp      non-blocking status LED
 *
 * Board: ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C3  (Arduino-ESP32 core 2.x or 3.x)
 * Serial monitor: 115200 baud, line ending "newline"
 */

#include "peer_config.h"
#include "console.h"
#include "led.h"
#include "link.h"
#include "protocol.h"

void setup() {
  Serial.begin(115200);
  delay(300);

  ledBegin();

  Serial.println();
  Serial.printf("=== ESP-NOW node %s, protocol v%d ===\n", NODE_NAME, PROTO_VERSION);

  linkBegin();

  Serial.printf("mac     : %s\n", linkOwnMac());
  Serial.printf("channel : %d\n", ESPNOW_CHANNEL);
  Serial.println(linkIsPaired() ? "peer    : configured" : "peer    : searching...");

  consoleBegin();
}

void loop() {
  linkService();      /* receive, retry, transmit */
  consoleService();   /* typed commands           */
  ledService();       /* blink patterns           */
}
