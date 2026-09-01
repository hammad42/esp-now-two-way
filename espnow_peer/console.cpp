#include <strings.h>   /* strcasecmp / strncasecmp */

#include "console.h"
#include "led.h"
#include "link.h"
#include "peer_config.h"
#include "protocol.h"

#define LINE_MAX 220

static char   line[LINE_MAX + 1];
static size_t lineLen = 0;


void consoleHelp() {
  Serial.println();
  Serial.println("commands");
  Serial.println("  send <text>      send a chat line to the other board");
  Serial.println("  led on|off       switch the other board LED");
  Serial.println("  led blink [n]    blink the other board LED n times (default 3)");
  Serial.println("  identify         make the other board blink a long burst");
  Serial.println("  ping             send one reading now and time the round trip");
  Serial.println("  auto on|off      start or stop the periodic reading");
  Serial.println("  interval <ms>    change the periodic reading interval");
  Serial.println("  peer             show the pairing state");
  Serial.println("  unpair           forget the peer and run discovery again");
  Serial.println("  stats            print counters, loss and round-trip times");
  Serial.println("  stats reset      clear the counters");
  Serial.println("  help             this list");
  Serial.println();
}

/* Skips leading blanks and returns nullptr when nothing is left. */
static const char *argAfter(const char *s) {
  while (*s == ' ' || *s == '\t') s++;
  return *s ? s : nullptr;
}

static void notPaired() {
  Serial.println("[warn] no peer yet - wait for the HELLO exchange, then try again");
}

static void execute(char *cmd) {
  if (!*cmd) return;
  Serial.printf("> %s\n", cmd);

  if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
    consoleHelp();

  } else if (!strcasecmp(cmd, "stats")) {
    linkPrintStats();

  } else if (!strcasecmp(cmd, "stats reset")) {
    linkResetStats();

  } else if (!strcasecmp(cmd, "peer")) {
    Serial.printf("[peer] this node %s is %s\n", linkOwnMac(),
                  linkIsPaired() ? "paired" : "unpaired, broadcasting HELLO");
    if (linkIsPaired()) Serial.printf("[peer] partner %s\n", linkMacToStr(linkPeerMac()));

  } else if (!strcasecmp(cmd, "unpair")) {
    linkUnpair();

  } else if (!strncasecmp(cmd, "send", 4)) {
    const char *text = argAfter(cmd + 4);
    if (!text) Serial.println("[warn] usage: send <text>");
    else if (!linkIsPaired()) notPaired();
    else if (!linkSendText(text)) Serial.println("[warn] outbox full, try again shortly");

  } else if (!strncasecmp(cmd, "led", 3)) {
    const char *arg = argAfter(cmd + 3);
    if (!arg) {
      Serial.println("[warn] usage: led on|off|blink [n]");
    } else if (!linkIsPaired()) {
      notPaired();
    } else if (!strcasecmp(arg, "on")) {
      linkSendCommand(CMD_LED_ON, 0);
    } else if (!strcasecmp(arg, "off")) {
      linkSendCommand(CMD_LED_OFF, 0);
    } else if (!strncasecmp(arg, "blink", 5)) {
      const char *n = argAfter(arg + 5);
      linkSendCommand(CMD_LED_BLINK, n ? (uint8_t)atoi(n) : 3);
    } else {
      Serial.println("[warn] usage: led on|off|blink [n]");
    }

  } else if (!strcasecmp(cmd, "identify")) {
    if (!linkIsPaired()) notPaired();
    else linkSendCommand(CMD_IDENTIFY, 0);

  } else if (!strcasecmp(cmd, "ping")) {
    if (!linkIsPaired()) notPaired();
    else if (!linkSendData(0.0f)) Serial.println("[warn] outbox full, try again shortly");

  } else if (!strncasecmp(cmd, "auto", 4)) {
    const char *arg = argAfter(cmd + 4);
    if (arg && !strcasecmp(arg, "on"))       linkSetAutoSend(true);
    else if (arg && !strcasecmp(arg, "off")) linkSetAutoSend(false);
    else { Serial.println("[warn] usage: auto on|off"); return; }
    Serial.printf("[conf] periodic reading %s\n", linkAutoSend() ? "on" : "off");

  } else if (!strncasecmp(cmd, "interval", 8)) {
    const char *arg = argAfter(cmd + 8);
    if (!arg) { Serial.println("[warn] usage: interval <ms>"); return; }
    linkSetInterval((uint32_t)atol(arg));
    Serial.printf("[conf] interval is now %lums\n", (unsigned long)linkInterval());

  } else {
    Serial.printf("[warn] unknown command: %s (try help)\n", cmd);
  }
}

void consoleBegin() {
  lineLen = 0;
  consoleHelp();
}

void consoleService() {
  while (Serial.available()) {
    const char c = (char)Serial.read();

    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = '\0';
      execute(line);
      lineLen = 0;
      continue;
    }
    if (c == '\b' || c == 127) {          /* backspace, for terminals that send it */
      if (lineLen) lineLen--;
      continue;
    }
    if (lineLen < LINE_MAX) line[lineLen++] = c;
  }
}
