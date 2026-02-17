#pragma once
/*
 * starlight-sys :: injector/src/arduino_serial.h
 *
 * Shared Arduino Starlink serial interface.
 * Used by both Triggerbot (clicks) and Aimbot (mouse movement).
 */

#include <Windows.h>

/* Open the Starlink Arduino on the given COM port (auto-detect if empty).
 * Returns true if connection is ready. Thread-safe (calls are serialized). */
bool ArduinoOpen(const char* comPort = nullptr);

/* Close the serial connection. */
void ArduinoClose();

/* Send a raw command string (e.g. "P,L\n", "M,3.50,-1.20\n").
 * Returns true on success. */
bool ArduinoSend(const char* cmd);

/* Returns true if the serial port is open and ready. */
bool ArduinoIsOpen();

/* Returns the detected COM port name (e.g. "COM4"), or "" if not connected. */
const char* ArduinoGetPort();
