/*
 * protocol.h - the bytes that go over the air.
 *
 * A fixed 20-byte header followed by 0-200 bytes of payload whose meaning
 * depends on the message type. Only header + payload_len bytes are ever
 * transmitted, so a HELLO costs 20 bytes and a full chat line costs 220.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <Arduino.h>

#define PROTO_VERSION      2
#define ESPNOW_MAX_PAYLOAD 200   /* ESP-NOW allows 250 bytes per frame in total */

/* Message types. */
enum : uint8_t {
  MSG_HELLO = 0,   /* discovery, broadcast, no payload            */
  MSG_DATA  = 1,   /* periodic reading, payload_data_t            */
  MSG_ACK   = 2,   /* acknowledgement, seq echoes the acked frame */
  MSG_TEXT  = 3,   /* chat line, payload is UTF-8, not terminated */
  MSG_CMD   = 4    /* remote command, payload_cmd_t               */
};

/* Header flags. */
enum : uint8_t {
  FLAG_NEEDS_ACK = 0x01,   /* sender expects an ACK and will retry without one */
  FLAG_RETRY     = 0x02    /* this is a retransmission, not a first attempt    */
};

/* Remote commands carried by MSG_CMD. */
enum : uint8_t {
  CMD_LED_ON    = 0,
  CMD_LED_OFF   = 1,
  CMD_LED_BLINK = 2,   /* arg = number of blinks */
  CMD_IDENTIFY  = 3    /* long blink burst, to tell two boards apart */
};

typedef struct __attribute__((packed)) {
  uint8_t  version;       /* PROTO_VERSION; other versions are dropped      */
  uint8_t  type;          /* MSG_*                                          */
  uint8_t  flags;         /* FLAG_*                                         */
  uint8_t  payload_len;   /* bytes of payload that follow, 0-200            */
  uint32_t seq;           /* per-sender counter; an ACK echoes it           */
  uint32_t uptime_ms;     /* millis() of the sender                         */
  char     name[8];       /* NODE_NAME of the sender, NUL padded            */
} espnow_header_t;        /* 20 bytes                                       */

typedef struct __attribute__((packed)) {
  espnow_header_t hdr;
  uint8_t         payload[ESPNOW_MAX_PAYLOAD];
} espnow_frame_t;

/* MSG_DATA payload. */
typedef struct __attribute__((packed)) {
  float value;
} payload_data_t;

/* MSG_CMD payload. */
typedef struct __attribute__((packed)) {
  uint8_t cmd;
  uint8_t arg;
} payload_cmd_t;

static inline uint16_t frameLength(const espnow_frame_t &f) {
  return (uint16_t)(sizeof(espnow_header_t) + f.hdr.payload_len);
}

const char *msgTypeName(uint8_t type);
const char *cmdName(uint8_t cmd);

#endif /* PROTOCOL_H */
