// Settings and running totals kept in the last 2 KB of flash (reserved by
// board_upload.maximum_size in platformio.ini). Pages are 1 KB on the STM32F051.
#pragma once

#include "hw.h"

#define SETTINGS_ADDR 0x0800F800UL
#define TOTALS_ADDR   0x0800FC00UL
#define PAGE_SIZE     1024

// ---------------------------------------------------------------- flash primitives

static void flashWait() {
  while (FLASH->SR & FLASH_SR_BSY) {}
  FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPERR;
}

static void flashUnlock() {
  if (FLASH->CR & FLASH_CR_LOCK) {
    FLASH->KEYR = 0x45670123;
    FLASH->KEYR = 0xCDEF89AB;
  }
}

static void flashLock() {
  FLASH->CR |= FLASH_CR_LOCK;
}

// Call between flashUnlock() and flashLock()
static void flashErasePage(uint32_t addr) {
  FLASH->CR |= FLASH_CR_PER;
  FLASH->AR = addr;
  FLASH->CR |= FLASH_CR_STRT;
  flashWait();
  FLASH->CR &= ~FLASH_CR_PER;
}

static void flashProgram(uint32_t addr, const uint16_t *halfwords, uint16_t count) {
  volatile uint16_t *target = (volatile uint16_t *)addr;
  FLASH->CR |= FLASH_CR_PG;
  for (uint16_t i = 0; i < count; i++) {
    target[i] = halfwords[i];
    flashWait();
  }
  FLASH->CR &= ~FLASH_CR_PG;
}

// ---------------------------------------------------------------- record logs

// A page holds a log of fixed-size records. Each save takes the next free slot, so the page is
// erased only once every few dozen saves. The marker is written last: a record cut short by a
// power loss has none and is skipped.
#define LOG_MARKER        0xA55A
#define LOG_BLANK         0xFFFF
#define LOG_MAX_HALFWORDS 24

struct FlashLog {
  uint32_t base;
  uint8_t halfwords;   // the record plus its marker
  uint8_t next;        // the next free slot
};

static uint8_t logSlots(const FlashLog &log) {
  return PAGE_SIZE / (log.halfwords * 2);
}

static const uint16_t *logSlot(const FlashLog &log, uint8_t i) {
  return (const uint16_t *)log.base + i * log.halfwords;
}

// Copies the newest valid record into data; false if there is none
static bool logLoad(FlashLog &log, void *data) {
  bool found = false;
  uint8_t slots = logSlots(log);
  for (log.next = 0; log.next < slots; log.next++) {
    const uint16_t *slot = logSlot(log, log.next);
    bool blank = true;
    for (uint8_t i = 0; i < log.halfwords; i++) blank = blank && slot[i] == LOG_BLANK;
    if (blank) break;
    if (slot[log.halfwords - 1] != LOG_MARKER) continue;
    memcpy(data, slot, (log.halfwords - 1) * 2);
    found = true;
  }
  return found;
}

// Without an erase this takes well under 1 ms; once the page is full it is erased first (~20 ms)
static void logAppend(FlashLog &log, const void *data) {
  uint16_t buffer[LOG_MAX_HALFWORDS];
  memcpy(buffer, data, (log.halfwords - 1) * 2);
  buffer[log.halfwords - 1] = LOG_MARKER;
  flashUnlock();
  if (log.next >= logSlots(log)) {
    flashErasePage(log.base);
    log.next = 0;
  }
  flashProgram((uint32_t)logSlot(log, log.next), buffer, log.halfwords);
  flashLock();
  log.next++;
}

static void logErase(FlashLog &log) {
  flashUnlock();
  flashErasePage(log.base);
  flashLock();
  log.next = 0;
}

// ---------------------------------------------------------------- settings

#define SETTINGS_MAGIC 0x5A05   // change when the layout changes: old settings are then ignored

// Version 0x5A04 kept a single record at the start of the page. Its first 18 bytes have the same
// layout as Settings below (its spare byte is now `page`, which it left at 0).
#define SETTINGS_MAGIC_V4 0x5A04
#define SETTINGS_V4_BYTES 18

enum ShuntId : uint8_t { SHUNT_R010, SHUNT_R100, SHUNT_R001, SHUNT_COUNT };
const uint32_t SHUNT_UOHM[SHUNT_COUNT] = {10000, 100000, 1000};
const char *const SHUNT_NAME[SHUNT_COUNT] = {"R010", "R100", "R001"};
const uint8_t SHUNT_DEFAULT_GAIN[SHUNT_COUNT] = {1, 3, 0};   // 8 A, 3.2 A, 40 A

// Sizes of the option lists in main.cpp, for checking what is loaded from flash
#define TRIGGER_LEVELS     14
#define PAGES              5
#define DIM_OPTIONS        5
#define AUTO_RESET_OPTIONS 4
#define FLASH_OPTIONS      3

struct Settings {
  uint16_t magic;
  uint8_t shunt;        // ShuntId
  uint8_t gain;         // 0..3 = +-40, 80, 160, 320 mV
  uint8_t fast;         // 1 = 0.5 ms shunt-only sampling
  uint8_t trigIndex;    // into TRIGGER_MA
  uint8_t autoArm;      // 1 = re-arm after every capture
  uint8_t timebase;     // into GRAPH_SLOT_MS
  uint8_t contrast;
  uint8_t backlight;    // 0..10
  uint8_t rotation;     // into ROTATIONS
  uint8_t vDecimals;    // 1..3 digits after the point on the METER page
  uint8_t iDecimals;    // 1..3, the current is shown in amps
  uint8_t page;         // the page shown at power-up: the last one used
  int16_t cal;          // current gain correction in 0.1 % steps
  int16_t tare;         // zero offset in shunt counts
  uint16_t alertMv;     // low-voltage alert below this, 0 = off
  uint8_t alertI;       // high-current alert: 0 = off, else TRIGGER_MA index + 1
  uint8_t alertFlash;   // backlight flashing: 0 off, 1 on alerts, 2 also on events
  uint8_t alertLatch;   // 1 = an alert stays until a button is pressed
  uint8_t dimIndex;     // into DIM_S: dim the backlight after this long without a button
  uint8_t autoReset;    // into AUTO_RESET_MIN: new session when a load starts after this idle
  uint8_t reserved;
};

static_assert(sizeof(Settings) % 2 == 0 && sizeof(Settings) / 2 + 1 <= LOG_MAX_HALFWORDS, "Settings size");

Settings cfg;
Settings savedCfg;
FlashLog settingsLog = {SETTINGS_ADDR, sizeof(Settings) / 2 + 1, 0};

void settingsDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = SETTINGS_MAGIC;
  cfg.shunt = SHUNT_R010;
  cfg.gain = SHUNT_DEFAULT_GAIN[SHUNT_R010];
  cfg.trigIndex = 7;   // 500 mA
  cfg.autoArm = 1;
  cfg.timebase = 2;    // 1 s per column
  cfg.contrast = 130;
  cfg.backlight = 6;
  cfg.rotation = 2;    // turned 180 degrees
  cfg.vDecimals = 2;
  cfg.iDecimals = 2;
  cfg.alertFlash = 1;  // flash on alerts; the alerts themselves start off
}

static bool settingsValid() {
  return cfg.magic == SETTINGS_MAGIC && cfg.shunt < SHUNT_COUNT && cfg.gain <= 3 && cfg.backlight <= 10 &&
         cfg.rotation <= 3 && cfg.cal >= -200 && cfg.cal <= 200 && cfg.vDecimals >= 1 && cfg.vDecimals <= 3 &&
         cfg.iDecimals >= 1 && cfg.iDecimals <= 3 && cfg.page < PAGES && cfg.trigIndex < TRIGGER_LEVELS &&
         cfg.alertI <= TRIGGER_LEVELS && cfg.alertFlash < FLASH_OPTIONS && cfg.alertLatch <= 1 &&
         cfg.dimIndex < DIM_OPTIONS && cfg.autoReset < AUTO_RESET_OPTIONS && cfg.alertMv <= 20000;
}

void settingsLoad() {
  settingsDefaults();
  Settings stored;
  if (logLoad(settingsLog, &stored)) {
    cfg = stored;
  } else if (*(const uint16_t *)SETTINGS_ADDR == SETTINGS_MAGIC_V4) {
    // Keep what the previous firmware stored; the new fields keep their defaults
    memcpy(&cfg, (const void *)SETTINGS_ADDR, SETTINGS_V4_BYTES);
    cfg.magic = SETTINGS_MAGIC;
    cfg.page = 0;
  }
  if (!settingsValid()) settingsDefaults();
  savedCfg = cfg;
}

// Writes only if something changed since the last save
void settingsSave() {
  if (memcmp(&cfg, &savedCfg, sizeof(cfg)) == 0) return;
  logAppend(settingsLog, &cfg);
  savedCfg = cfg;
}

// ---------------------------------------------------------------- totals log

struct StoredTotals {
  int64_t chargeUaS;
  int64_t energyUwS;
  uint32_t seconds;
  int32_t peakUa;
  uint16_t vMin, vMax;
};

static_assert(sizeof(StoredTotals) % 2 == 0 && sizeof(StoredTotals) / 2 + 1 <= LOG_MAX_HALFWORDS, "Totals size");

FlashLog totalsLog = {TOTALS_ADDR, sizeof(StoredTotals) / 2 + 1, 0};

// The newest valid record, false if there is none
bool totalsLoad(StoredTotals &t) {
  return logLoad(totalsLog, &t);
}

void totalsSave(const StoredTotals &t) {
  logAppend(totalsLog, &t);
}

void totalsErase() {
  logErase(totalsLog);
}
