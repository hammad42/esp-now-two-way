/*
 * link.h - the ESP-NOW transport: pairing, retries, statistics.
 *
 * Everything here is driven from loop() by linkService(). The ESP-NOW
 * callbacks only queue work, they never send or print.
 */
#ifndef LINK_H
#define LINK_H

#include <Arduino.h>

typedef struct {
  uint32_t txFrames;      /* frames handed to the radio, retries included     */
  uint32_t rxFrames;      /* valid frames received                            */
  uint32_t acked;         /* frames confirmed by an application-level ACK     */
  uint32_t retries;       /* retransmissions sent                             */
  uint32_t dropped;       /* frames given up on after the last retry          */
  uint32_t duplicates;    /* retransmissions we had already seen              */
  uint32_t radioOk;       /* link-layer ACKs from the send callback           */
  uint32_t radioFail;     /* frames the radio could not deliver               */
  uint32_t rxQueueDrops;  /* frames lost because loop() fell behind           */
  uint32_t txQueueDrops;  /* sends refused because the outbox was full        */
  uint32_t rttMin, rttMax, rttLast;
  uint64_t rttSum;
  uint32_t rttCount;
  int16_t  lastRssi;      /* dBm of the last frame, 1 when unavailable        */
} link_stats_t;

void linkBegin();
void linkService();

bool linkSendData(float value);
bool linkSendText(const char *text);
bool linkSendCommand(uint8_t cmd, uint8_t arg);

bool           linkIsPaired();
const uint8_t *linkPeerMac();
const char    *linkOwnMac();
const char    *linkMacToStr(const uint8_t *mac);   /* shared formatter, static buffer */
void           linkUnpair();

void     linkSetAutoSend(bool on);
bool     linkAutoSend();
void     linkSetInterval(uint32_t ms);
uint32_t linkInterval();

const link_stats_t *linkStats();
void                linkResetStats();
void                linkPrintStats();

#endif /* LINK_H */
