// PC stand-in for include/hw.h: the same helpers, no hardware
#pragma once

#include "Arduino.h"

#define PIN_LCD_CS  0
#define PIN_LCD_DC  0
#define PIN_LCD_RST 0
#define PIN_LCD_LED 0
#define PIN_LEFT    0
#define PIN_CENTER  1
#define PIN_RIGHT   2

inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
inline int32_t absolute(int32_t v) { return v < 0 ? -v : v; }

inline void setBacklight(uint8_t) {}
inline void backlightCut() {}
inline void watchdogStart() {}
inline void watchdogKick() {}
inline bool resetByWatchdog() { return false; }

// store.h programs the flash through these; the screenshots never call them
struct FakeFlash { volatile uint32_t SR, CR, KEYR, AR; };
static FakeFlash fakeFlash;
#define FLASH (&fakeFlash)
#define FLASH_SR_BSY 1
#define FLASH_SR_EOP 2
#define FLASH_SR_PGERR 4
#define FLASH_SR_WRPERR 8
#define FLASH_CR_LOCK 1
#define FLASH_CR_PER 2
#define FLASH_CR_STRT 4
#define FLASH_CR_PG 8
