/*
 * led.h - non-blocking status LED.
 *
 * Nothing here ever blocks: ledBlink() only sets up a pattern that
 * ledService() steps through from loop().
 */
#ifndef LED_H
#define LED_H

#include <Arduino.h>

void ledBegin();
void ledService();

void ledSet(bool on);         /* steady state the LED returns to after blinking */
void ledBlink(uint8_t times); /* blink now, then go back to the steady state    */
bool ledSteadyState();

#endif /* LED_H */
