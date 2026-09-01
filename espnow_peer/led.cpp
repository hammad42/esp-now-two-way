#include "led.h"
#include "peer_config.h"

#define BLINK_ON_MS  60
#define BLINK_OFF_MS 120

static bool     steadyOn      = false;
static uint8_t  blinksLeft    = 0;
static bool     blinkPhaseOn  = false;
static uint32_t phaseUntilMs  = 0;

static void write(bool on) {
#if STATUS_LED_PIN >= 0
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

void ledBegin() {
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
#endif
  write(false);
}

void ledSet(bool on) {
  steadyOn = on;
  if (blinksLeft == 0) write(on);
}

void ledBlink(uint8_t times) {
  if (times == 0) return;
  blinksLeft   = times;
  blinkPhaseOn = true;
  phaseUntilMs = millis() + BLINK_ON_MS;
  write(true);
}

bool ledSteadyState() {
  return steadyOn;
}

void ledService() {
  if (blinksLeft == 0) return;
  if ((int32_t)(millis() - phaseUntilMs) < 0) return;

  if (blinkPhaseOn) {
    write(false);
    blinkPhaseOn = false;
    phaseUntilMs = millis() + BLINK_OFF_MS;
    if (--blinksLeft == 0) write(steadyOn);   /* pattern done, restore steady */
  } else {
    write(true);
    blinkPhaseOn = true;
    phaseUntilMs = millis() + BLINK_ON_MS;
  }
}
