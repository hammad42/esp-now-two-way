/*
 * get_mac_address.ino - prints the station MAC of this board.
 *
 * Only needed when PEER_AUTO_DISCOVER is set to 0 in peer_config.h: flash this
 * to a board, copy the printed address into the other board PEER_MAC_ADDRESS
 * line, then flash espnow_peer back over it.
 */

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(300);
  WiFi.mode(WIFI_STA);

  Serial.println();
  Serial.print("STA MAC : ");
  Serial.println(WiFi.macAddress());

  uint8_t mac[6];
  WiFi.macAddress(mac);
  Serial.printf("PEER_MAC_ADDRESS { 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X }\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void loop() {
  delay(5000);
}
