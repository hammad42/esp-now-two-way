/*
 * peer_config.h — the only file you normally need to touch.
 */
#ifndef PEER_CONFIG_H
#define PEER_CONFIG_H

/* ---------------------------------------------------------------------------
 * Node identity
 * Flash the SAME sketch to both boards, changing only this line:
 *   board 1 -> "A"      board 2 -> "B"
 * The name is only used for readable serial output.
 * ------------------------------------------------------------------------ */
#define NODE_NAME "A"

/* ---------------------------------------------------------------------------
 * Radio
 * Both boards must sit on the same channel (1-13). Channel 1 is a safe default.
 * ------------------------------------------------------------------------ */
#define ESPNOW_CHANNEL 1

/* How often a node transmits, in milliseconds. */
#define SEND_INTERVAL_MS 2000

/* ---------------------------------------------------------------------------
 * Peering
 * PEER_AUTO_DISCOVER 1 -> boards find each other with a broadcast HELLO.
 *                        Nothing to configure, just power both up.
 * PEER_AUTO_DISCOVER 0 -> use the fixed MAC below. Get it by flashing
 *                        tools/get_mac_address to the *other* board.
 * ------------------------------------------------------------------------ */
#define PEER_AUTO_DISCOVER 1
#define PEER_MAC_ADDRESS { 0x24, 0x6F, 0x28, 0x00, 0x00, 0x00 }

/* ---------------------------------------------------------------------------
 * Optional payload encryption (AES-128-CCM, done by the ESP-NOW stack).
 * Set to 1 on BOTH boards and change the keys. Keys must be exactly 16 bytes.
 * Discovery HELLOs are broadcast and therefore always sent in the clear;
 * only the unicast DATA/ACK traffic is encrypted.
 * ------------------------------------------------------------------------ */
#define ENABLE_ENCRYPTION 0
#define ESPNOW_PMK "pmk_change_me_16"
#define ESPNOW_LMK "lmk_change_me_16"

/* Blink the on-board LED on every received frame (set to -1 to disable). */
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
#define STATUS_LED_PIN LED_BUILTIN

#endif /* PEER_CONFIG_H */
