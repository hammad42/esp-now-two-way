/*
 * console.h - line-oriented command interface on the serial port.
 *
 * Type commands into the Arduino serial monitor (line ending: newline) to
 * chat with the other board, drive its LED, or inspect the link. Reading is
 * non-blocking: consoleService() consumes whatever bytes have arrived.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <Arduino.h>

void consoleBegin();
void consoleService();
void consoleHelp();

#endif /* CONSOLE_H */
