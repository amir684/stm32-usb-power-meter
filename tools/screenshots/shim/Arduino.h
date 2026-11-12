// Just enough of Arduino.h to compile src/main.cpp on a PC for the screenshots
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define INPUT_PULLUP 2

// Macros, like the Arduino core: main.cpp mixes int32_t and long
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define constrain(x, low, high) ((x) < (low) ? (low) : ((x) > (high) ? (high) : (x)))

extern uint32_t simMillis;
inline uint32_t millis() { return simMillis; }
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}
inline void pinMode(uint32_t, uint32_t) {}
inline int digitalRead(uint32_t) { return HIGH; }
inline void noInterrupts() {}
inline void interrupts() {}
