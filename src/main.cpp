// USB hub power meter: INA219 + ST7567 128x64 LCD + 3-button joystick on a bare STM32F051K8T6.
// Wiring: include/hw.h
//
// Pages (left / right):  METER   volts, amps, watts and the peak
//                        GRAPH   average current history, 0.05 s .. 30 s per column
//                        VOLTS   average voltage history; the lowest reading as a number
//                        SPIKE   triggered capture of millisecond current spikes, with the voltage
//                        TOTALS  mAh, Wh, time, average, voltage sag, peak
// Center click: menu. Center hold: quick action - METER reset peaks, GRAPH / VOLTS next time span,
//                                            SPIKE arm the trigger, TOTALS reset the totals.
// In the menu: left / right move, center selects or edits, hold center to leave.
// While the backlight is dimmed, or an alert is latched, the first press only wakes / clears.

#include <SPI.h>
#include <U8g2lib.h>

#include "hw.h"
#include "ina219.h"
#include "store.h"

U8G2_ST7567_JLX12864_F_4W_HW_SPI lcd(U8G2_R0, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);

const u8g2_cb_t *const ROTATIONS[4] = {U8G2_R0, U8G2_MIRROR, U8G2_R2, U8G2_MIRROR_VERTICAL};
const char *const ROTATION_NAME[4] = {"Normal", "Mirror", "Turn 180", "Mirror V"};

#define FONT_SMALL u8g2_font_5x8_tr
#define FONT_TEXT  u8g2_font_6x10_tr
#define FONT_BIG   u8g2_font_logisoso20_tn

#define FRAME_MS      40
#define COLLECT_MS    10
#define DISPLAY_MS    250
#define COUNT_MIN_UA  5000L      // the totals only advance at or above 5 mA
#define SAVE_PERIOD_MS 120000UL
#define SAVE_MIN_UA_S 3600000LL  // 1 mAh: smaller changes are not worth wearing out the flash

const uint16_t TRIGGER_MA[] = {5, 10, 20, 50, 100, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 5000};
#define TRIGGER_COUNT (sizeof(TRIGGER_MA) / sizeof(TRIGGER_MA[0]))
static_assert(TRIGGER_COUNT == TRIGGER_LEVELS, "store.h checks the trigger index against TRIGGER_LEVELS");

// Backlight dimming after the last button press, in seconds (0 = never)
const uint16_t DIM_S[DIM_OPTIONS] = {0, 30, 60, 300, 900};
const char *const DIM_NAME[DIM_OPTIONS] = {"Never", "30s", "1min", "5min", "15min"};
#define DIM_LEVEL 3   // backlight level while dimmed (level 1 is already dark)

// A load that starts after this much idle time begins a new session: totals and peaks reset
const uint8_t AUTO_RESET_MIN[AUTO_RESET_OPTIONS] = {0, 1, 10, 60};
const char *const AUTO_RESET_NAME[AUTO_RESET_OPTIONS] = {"Off", "1min", "10min", "1h"};

const char *const FLASH_NAME[FLASH_OPTIONS] = {"Off", "Alerts", "All"};
#define FLASH_HALF_MS  70     // fast flashing: 70 ms on, 70 ms off
#define ALERT_HOLD_MS  2000   // an alert stays this long after its cause is gone
#define ALERT_LOW_V    1
#define ALERT_HIGH_I   2

#define PAGE_SAVE_MS     10000   // the page is remembered once it has been shown this long
#define POWER_REARM_MS   10000   // at most one power-loss save per 10 s

const uint16_t GRAPH_SLOT_MS[] = {50, 200, 1000, 5000, 30000};
const char *const GRAPH_SPAN[] = {"6.4s", "26s", "2min", "11min", "64min"};   // 128 columns
#define TIMEBASE_COUNT (sizeof(GRAPH_SLOT_MS) / sizeof(GRAPH_SLOT_MS[0]))

#define GRAPH_W 128

// ---------------------------------------------------------------- measurement state

int32_t uaPerCountQ8;   // microamps per shunt count, times 256

int64_t rawToUa(int64_t raw) {
  return raw * uaPerCountQ8 / 256;
}

int32_t uaToRaw(int64_t ua) {
  return ua * 256 / uaPerCountQ8;
}

bool sensorOk;
uint32_t nextReconnect;

int32_t shownUa, shownMv, shownRaw;   // averaged over DISPLAY_MS
int64_t dispSum;
uint32_t dispCount, dispMvSum, dispMvCount, nextDisplay;
uint32_t overloadUntil;

int64_t chargeUaMs, energyUwMs;
uint64_t countedUs;
bool counting;
int32_t peakUa;
uint16_t vMin = 0xFFFF, vMax;
int32_t savedChargeUaS;
uint32_t nextSave;

bool wasCounting;
uint32_t idleSince;   // when the load last stopped (or power-up)

int32_t secMaxUa, lastSecMaxUa;
uint32_t secEnd;

int16_t graphAvg[GRAPH_W], graphPeak[GRAPH_W];   // shunt counts; the peak is a magnitude
uint16_t graphVLow[GRAPH_W], graphVAvg[GRAPH_W];   // mV; 0 average marks a column without voltage
uint8_t graphHead, graphFill;
int64_t slotSum;
uint32_t slotCount, slotEnd;
int16_t slotPeak;
uint16_t slotVLow;
uint32_t slotVSum, slotVCount;

int16_t shot[CAP_LEN];   // the last capture, oldest sample first
uint16_t shotMv[CAP_LEN];
bool haveShot, shotHasV;
int32_t shotPeakUa;
uint32_t shotWidthUs;
uint16_t shotTickUs, shotMinMv, shotMaxMv;

uint8_t alertBits;    // ALERT_LOW_V | ALERT_HIGH_I: shown in the header and flashed
uint32_t alertUntil;  // without latching, the alert ends here
uint32_t flashUntil;  // a short burst of flashes for an event
uint32_t lastPressMs;

bool powerLow;        // a power loss was seen and the voltage has not come back yet
uint32_t powerFailAt;

void toast(const char *text, uint32_t now);

void updateScale() {
  // 10 uV per count: I = counts * 10 uV / R, then the gain correction
  uaPerCountQ8 = (int32_t)(2560000ULL * (uint32_t)(1000 + cfg.cal) / SHUNT_UOHM[cfg.shunt]);
}

int32_t triggerUa() {
  return TRIGGER_MA[cfg.trigIndex] * 1000L;
}

void applyTrigger() {
  setTrigger(uaToRaw(triggerUa()));
}

void clearGraph() {
  memset(graphAvg, 0, sizeof(graphAvg));
  memset(graphPeak, 0, sizeof(graphPeak));
  graphHead = graphFill = 0;
  slotSum = slotCount = 0;
  slotPeak = 0;
  slotVLow = 0xFFFF;
  slotVSum = slotVCount = 0;
  slotEnd = millis() + GRAPH_SLOT_MS[cfg.timebase];
}

void resetPeaks() {
  peakUa = 0;
  vMin = 0xFFFF;
  vMax = 0;
  spikeCount = 0;
  haveShot = false;
}

void resetTotals() {
  chargeUaMs = energyUwMs = 0;
  countedUs = 0;
  savedChargeUaS = 0;
  totalsErase();
}

void newSession(uint32_t now) {
  resetTotals();
  resetPeaks();
  toast("New session", now);
}

void saveTotals() {
  StoredTotals t = {chargeUaMs / 1000, energyUwMs / 1000, (uint32_t)(countedUs / 1000000), peakUa, vMin, vMax};
  totalsSave(t);
  savedChargeUaS = t.chargeUaS;
}

void loadTotals() {
  StoredTotals t;
  if (totalsLoad(t)) {
    chargeUaMs = t.chargeUaS * 1000;
    energyUwMs = t.energyUwS * 1000;
    countedUs = (uint64_t)t.seconds * 1000000;
    peakUa = t.peakUa;
    vMin = t.vMin;
    vMax = t.vMax;
    savedChargeUaS = t.chargeUaS;
  }
}

// Reconfigures the sensor after a change of range, shunt or sampling rate
void restartSensor() {
  samplerStop();
  tareRaw = cfg.tare;
  applyTrigger();
  sensorOk = samplerStart(cfg.gain, cfg.fast);
  samplerCollect();   // drop what was gathered with the old settings
  captureArm();
  nextReconnect = millis() + 1000;
}

void collect(uint32_t now) {
  SampleWindow w = samplerCollect();
  if (w.overload) overloadUntil = now + 1000;
  if (!w.count) return;

  int32_t magnitude = max(absolute(w.low), absolute(w.high));
  int32_t peak = rawToUa(magnitude);
  if (peak > peakUa) peakUa = peak;
  if (peak > secMaxUa) secMaxUa = peak;
  bool haveV = w.mvLow <= w.mvHigh;   // false at 0.5 ms sampling: the voltage is only held

  // Alerts look at every single sample, so a 1 ms spike or dip is enough
  uint8_t hit = 0;
  if (cfg.alertI && peak >= TRIGGER_MA[cfg.alertI - 1] * 1000L) hit |= ALERT_HIGH_I;
  if (cfg.alertMv && haveV && w.mvLow < cfg.alertMv) hit |= ALERT_LOW_V;
  if (hit) {
    alertBits |= hit;
    alertUntil = now + ALERT_HOLD_MS;
  }

  dispSum += w.sum;
  dispCount += w.count;
  if (w.mv) {
    dispMvSum += w.mv;
    dispMvCount++;
  }

  int64_t chargeStep = rawToUa(w.sum) * tickUs / 1000;   // uA * ms
  counting = absolute(rawToUa(w.sum) / w.count) >= COUNT_MIN_UA;
  if (counting && !wasCounting && cfg.autoReset && countedUs &&
      now - idleSince >= AUTO_RESET_MIN[cfg.autoReset] * 60000UL) {
    newSession(now);
  }
  if (wasCounting && !counting) idleSince = now;
  wasCounting = counting;

  if (counting) {
    chargeUaMs += chargeStep;
    energyUwMs += chargeStep * w.mv / 1000;
    countedUs += (uint32_t)w.count * tickUs;
    if (haveV) {   // the voltage under load, every sample: shows how far the supply sags
      if (w.mvLow < vMin) vMin = w.mvLow;
      if (w.mvHigh > vMax) vMax = w.mvHigh;
    }
  }

  slotSum += w.sum;
  slotCount += w.count;
  if (magnitude > slotPeak) slotPeak = magnitude;
  uint16_t vLow = haveV ? w.mvLow : w.mv;
  if (vLow < slotVLow) slotVLow = vLow;
  slotVSum += w.mv;   // one reading per 10 ms window: plenty for an average
  slotVCount++;
}

void updateDerived(uint32_t now) {
  if (due(now, nextDisplay)) {
    nextDisplay = now + DISPLAY_MS;
    if (dispCount) {
      shownUa = rawToUa(dispSum) / (int32_t)dispCount;
      shownRaw = dispSum / (int32_t)dispCount;
    }
    if (dispMvCount) shownMv = dispMvSum / dispMvCount;
    if (!sensorOk) shownUa = shownMv = shownRaw = 0;   // old readings must not stay on the screen
    dispSum = dispCount = dispMvSum = dispMvCount = 0;
  }

  if (due(now, slotEnd)) {
    slotEnd = now + GRAPH_SLOT_MS[cfg.timebase];
    graphAvg[graphHead] = slotCount ? slotSum / (int32_t)slotCount : 0;
    graphPeak[graphHead] = slotPeak;
    graphVAvg[graphHead] = slotVCount ? slotVSum / slotVCount : 0;
    graphVLow[graphHead] = min(slotVLow, graphVAvg[graphHead]);
    graphHead = (graphHead + 1) % GRAPH_W;
    if (graphFill < GRAPH_W) graphFill++;
    slotSum = slotCount = 0;
    slotPeak = 0;
    slotVLow = 0xFFFF;
    slotVSum = slotVCount = 0;
  }

  if (alertBits && !cfg.alertLatch && due(now, alertUntil)) alertBits = 0;

  if (due(now, secEnd)) {
    secEnd = now + 1000;
    lastSecMaxUa = secMaxUa;
    secMaxUa = 0;
  }

  if (capState == CAP_DONE) {
    int32_t peakRaw = 0;
    shotMinMv = 0xFFFF;
    shotMaxMv = 0;
    for (uint8_t i = 0; i < CAP_LEN; i++) {
      uint8_t k = (capStart + i) & (CAP_LEN - 1);
      shot[i] = cap[k];
      shotMv[i] = capMv[k];
      peakRaw = max(peakRaw, absolute(shot[i]));
      shotMinMv = min(shotMinMv, shotMv[i]);
      shotMaxMv = max(shotMaxMv, shotMv[i]);
    }
    shotHasV = !fastMode;
    // Width: how long the current stayed above the trigger's release level
    uint8_t n = CAP_PRE;
    while (n < CAP_LEN && absolute(shot[n]) >= trigLowRaw) n++;
    shotTickUs = tickUs;
    shotWidthUs = (uint32_t)(n - CAP_PRE) * tickUs;
    shotPeakUa = rawToUa(peakRaw);
    haveShot = true;
    if (cfg.autoArm) {
      captureArm();
    } else {
      capState = CAP_IDLE;
      flashUntil = now + 600;   // the single shot you were waiting for
    }
  }

  if (due(now, nextSave)) {
    nextSave = now + SAVE_PERIOD_MS;
    if (llabs(chargeUaMs / 1000 - savedChargeUaS) >= SAVE_MIN_UA_S) saveTotals();
  }
}

// ---------------------------------------------------------------- text helpers

char *putUint(char *p, uint32_t value, uint8_t minDigits) {
  char reversed[10];
  uint8_t n = 0;
  do {
    reversed[n++] = '0' + value % 10;
    value /= 10;
  } while (value);
  while (n < minDigits) reversed[n++] = '0';
  while (n) *p++ = reversed[--n];
  *p = 0;
  return p;
}

char *putFixed(char *p, int32_t value, uint8_t decimals) {
  if (value < 0) {
    *p++ = '-';
    value = -value;
  }
  uint32_t scale = 1;
  for (uint8_t i = 0; i < decimals; i++) scale *= 10;
  p = putUint(p, value / scale, 1);
  if (decimals) {
    *p++ = '.';
    p = putUint(p, value % scale, decimals);
  }
  return p;
}

char *putText(char *p, const char *s) {
  while (*s) *p++ = *s++;
  *p = 0;
  return p;
}

// Rounds value / divisor to the nearest integer, also for negative values
int32_t roundDiv(int32_t value, int32_t divisor) {
  return value >= 0 ? (value + divisor / 2) / divisor : -((-value + divisor / 2) / divisor);
}

// "523.4mA", "1.234A", "12.34A"
char *putCurrent(char *p, int32_t ua) {
  if (absolute(ua) < 1000000) {
    if (cfg.shunt == SHUNT_R001) p = putFixed(p, roundDiv(ua, 1000), 0);
    else p = putFixed(p, roundDiv(ua, 100), 1);
    return putText(p, "mA");
  }
  if (absolute(ua) < 10000000) p = putFixed(p, roundDiv(ua, 1000), 3);
  else p = putFixed(p, roundDiv(ua, 10000), 2);
  return putText(p, "A");
}

// Short labels: "50mA", "1.5A", "32A"
char *putAmpsShort(char *p, int32_t ua) {
  if (ua < 1000000) return putText(putUint(p, roundDiv(ua, 1000), 1), "mA");
  if (ua < 10000000) {
    int32_t tenths = roundDiv(ua, 100000);
    p = tenths % 10 ? putFixed(p, tenths, 1) : putUint(p, tenths / 10, 1);
  } else {
    p = putUint(p, roundDiv(ua, 1000000), 1);
  }
  return putText(p, "A");
}

char *putVolts(char *p, int32_t mv) {
  p = mv < 10000 ? putFixed(p, mv, 3) : putFixed(p, roundDiv(mv, 10), 2);
  return putText(p, "V");
}

char *putWatts(char *p, int32_t uw) {
  if (absolute(uw) < 1000000) return putText(putFixed(p, roundDiv(uw, 1000), 0), "mW");
  p = absolute(uw) < 10000000 ? putFixed(p, roundDiv(uw, 1000), 3) : putFixed(p, roundDiv(uw, 10000), 2);
  return putText(p, "W");
}

// "4.71V": two decimals, for labels
char *putVoltsShort(char *p, int32_t mv) {
  return putText(putFixed(p, roundDiv(mv, 10), 2), "V");
}

char *putDuration(char *p, uint32_t us) {
  if (us < 10000) return putText(putFixed(p, us / 100, 1), "ms");
  return putText(putUint(p, us / 1000, 1), "ms");
}

void drawRight(int right, int baseline, const char *s) {
  lcd.drawStr(right - lcd.getStrWidth(s), baseline, s);
}

void drawCentered(int baseline, const char *s) {
  lcd.drawStr((128 - lcd.getStrWidth(s)) / 2, baseline, s);
}

// Text on a white patch, readable on top of a graph
void drawLabel(int x, int baseline, const char *s, bool alignRight) {
  lcd.setFont(FONT_SMALL);
  int width = lcd.getStrWidth(s);
  if (alignRight) x -= width;
  lcd.setDrawColor(0);
  lcd.drawBox(x - 1, baseline - 7, width + 2, 9);
  lcd.setDrawColor(1);
  lcd.drawStr(x, baseline, s);
}

// 1, 2, 5 steps: the top of a graph scale
uint32_t niceCeil(uint32_t x) {
  for (uint32_t d = 1000;; d *= 10) {
    if (x <= d) return d;
    if (x <= 2 * d) return 2 * d;
    if (x <= 5 * d) return 5 * d;
  }
}

// ---------------------------------------------------------------- pages

enum Page : uint8_t { PAGE_METER, PAGE_GRAPH, PAGE_VOLTS, PAGE_SPIKE, PAGE_TOTALS, PAGE_COUNT };
static_assert(PAGE_COUNT == PAGES, "store.h checks the saved page against PAGES");
uint8_t page, lastPage;
uint32_t pageSaveAt;
bool blinkOn;

// Black bar: title, a tag, page dots. A missing sensor, an overload or an alert replaces the tag,
// blinking.
void drawHeader(const char *title, const char *tag, uint32_t now) {
  lcd.drawBox(0, 0, 128, 9);
  lcd.setDrawColor(0);
  lcd.setFont(FONT_SMALL);
  int x = 2 + lcd.drawStr(2, 7, title) + 5;

  const char *warning = !sensorOk                                  ? "NO SENSOR"
                        : !due(now, overloadUntil)                  ? "OVERLOAD"
                        : alertBits == (ALERT_LOW_V | ALERT_HIGH_I) ? "LOW V HIGH I"
                        : alertBits == ALERT_LOW_V                  ? "LOW V"
                        : alertBits                                 ? "HIGH I"
                                                                    : nullptr;
  if (warning) {
    if (blinkOn) lcd.drawStr(x, 7, warning);
  } else if (tag) {
    lcd.drawStr(x, 7, tag);
  }

  for (uint8_t i = 0; i < PAGE_COUNT; i++) {
    int cx = 128 - 6 * (PAGE_COUNT - i) + 1;
    if (i == page) lcd.drawDisc(cx, 4, 2);
    else lcd.drawPixel(cx, 4);
  }
  lcd.setDrawColor(1);
}

// Thousandths (mV or mA) with 1..3 digits after the point, rounded
void putThousandths(char *text, int32_t milli, uint8_t decimals) {
  static const int32_t DIV[4] = {1000, 100, 10, 1};
  putFixed(text, roundDiv(milli, DIV[decimals]), decimals);
}

void putDashes(char *text, uint8_t decimals) {
  char *p = putText(text, "-.");
  while (decimals--) *p++ = '-';
  *p = 0;
}

void pageMeter(uint32_t now) {
  char text[16];
  drawHeader("METER", fastMode ? "0.5ms" : nullptr, now);

  lcd.setFont(FONT_BIG);
  if (!sensorOk) putDashes(text, cfg.vDecimals);
  else putThousandths(text, shownMv, cfg.vDecimals);
  drawRight(111, 32, text);

  if (!sensorOk) putDashes(text, cfg.iDecimals);
  else putThousandths(text, roundDiv(shownUa, 1000), cfg.iDecimals);
  drawRight(111, 56, text);

  lcd.setFont(FONT_TEXT);
  lcd.drawStr(115, 32, "V");
  lcd.drawStr(115, 56, "A");
  if (fastMode) lcd.drawStr(115, 22, "h");   // "held": the voltage is not measured at 0.5 ms

  lcd.setFont(FONT_SMALL);
  putWatts(text, (int64_t)shownMv * shownUa / 1000);
  lcd.drawStr(0, 63, text);
  char *p = putText(text, "max ");
  putCurrent(p, peakUa);
  drawRight(128, 63, text);
}

// Both history pages: one line through the column averages, newest on the right, between a
// row of labels above (scale top, value now) and below (scale bottom, extreme on the screen).
const int PLOT_TOP = 19, PLOT_BOTTOM = 54, PLOT_H = PLOT_BOTTOM - PLOT_TOP;

// Index of the k-th oldest column on the screen, and its x position
uint8_t graphIndex(uint8_t k) {
  return (graphHead + GRAPH_W - graphFill + k) % GRAPH_W;
}

void plotLine(int x, int y, int &lastX, int &lastY) {
  if (lastX >= 0) lcd.drawLine(lastX, lastY, x, y);
  else lcd.drawPixel(x, y);
  lastX = x;
  lastY = y;
}

void plotLabels(const char *topLeft, const char *topRight, const char *bottomLeft, const char *bottomRight) {
  lcd.setFont(FONT_SMALL);
  lcd.drawStr(0, 17, topLeft);
  drawRight(128, 17, topRight);
  lcd.drawStr(0, 63, bottomLeft);
  drawRight(128, 63, bottomRight);
}

void pageGraph(uint32_t now) {
  drawHeader("GRAPH", GRAPH_SPAN[cfg.timebase], now);
  if (!graphFill) {
    lcd.setFont(FONT_TEXT);
    drawCentered(40, "collecting...");
    return;
  }

  int32_t avgRaw = 1, peakRaw = 0;
  for (uint8_t k = 0; k < graphFill; k++) {
    uint8_t i = graphIndex(k);
    avgRaw = max(avgRaw, absolute(graphAvg[i]));
    peakRaw = max(peakRaw, (int32_t)graphPeak[i]);
  }
  uint32_t scaleUa = niceCeil(rawToUa(avgRaw));
  int32_t scaleRaw = max(uaToRaw(scaleUa), 1L);

  for (int x = 0; x < 128; x += 3) lcd.drawPixel(x, PLOT_BOTTOM);   // zero
  int lastX = -1, lastY = 0;
  for (uint8_t k = 0; k < graphFill; k++) {
    int32_t v = min(absolute(graphAvg[graphIndex(k)]), scaleRaw);
    plotLine(GRAPH_W - graphFill + k, PLOT_BOTTOM - v * PLOT_H / scaleRaw, lastX, lastY);
  }

  char top[12], now_[12], peak[16];
  putAmpsShort(top, scaleUa);
  putCurrent(now_, shownUa);
  putCurrent(putText(peak, "max "), rawToUa(peakRaw));
  plotLabels(top, now_, "0", peak);
}

void pageVolts(uint32_t now) {
  drawHeader("VOLTS", GRAPH_SPAN[cfg.timebase], now);

  uint16_t lo = 0xFFFF, hi = 0, lowest = 0xFFFF;
  for (uint8_t k = 0; k < graphFill; k++) {
    uint8_t i = graphIndex(k);
    if (!graphVAvg[i]) continue;
    lo = min(lo, graphVAvg[i]);
    hi = max(hi, graphVAvg[i]);
    lowest = min(lowest, graphVLow[i]);
  }
  if (lo > hi) {
    lcd.setFont(FONT_TEXT);
    drawCentered(40, "collecting...");
    return;
  }

  // 50 mV grid, at least 200 mV tall so a few mV of noise stay a flat line
  int32_t lowMv = lo / 50 * 50, highMv = (hi + 49) / 50 * 50;
  while (highMv - lowMv < 200) {
    highMv += 50;
    if (highMv - lowMv < 200) lowMv -= 50;
  }
  if (lowMv < 0) {
    highMv -= lowMv;
    lowMv = 0;
  }
  int32_t span = highMv - lowMv;

  if (cfg.alertMv > lowMv && cfg.alertMv < highMv) {   // the alert level, dashed
    int y = PLOT_BOTTOM - (cfg.alertMv - lowMv) * PLOT_H / span;
    for (int x = 0; x < 128; x += 6) lcd.drawHLine(x, y, 3);
  }

  int lastX = -1, lastY = 0;
  for (uint8_t k = 0; k < graphFill; k++) {
    uint8_t i = graphIndex(k);
    if (!graphVAvg[i]) {
      lastX = -1;
      continue;
    }
    plotLine(GRAPH_W - graphFill + k, PLOT_BOTTOM - (graphVAvg[i] - lowMv) * PLOT_H / span, lastX, lastY);
  }

  char top[12], now_[12], bottom[12], low[16];
  putVoltsShort(top, highMv);
  if (fastMode) strcpy(now_, "held");
  else putVolts(now_, shownMv);
  putVoltsShort(bottom, lowMv);
  putVoltsShort(putText(low, "min "), lowest);   // the lowest single reading on the screen
  plotLabels(top, now_, bottom, low);
}

void pageSpike(uint32_t now) {
  char text[24];
  char *p = text;
  if (capState == CAP_ARMED) p = putText(p, "ARMED ");
  else if (capState == CAP_CAPTURING) p = putText(p, "TRIG ");
  else p = putText(p, "HOLD ");
  p = putText(p, ">");
  putAmpsShort(p, triggerUa());
  drawHeader("SPIKE", text, now);

  lcd.setFont(FONT_SMALL);
  p = putText(text, "count ");
  putUint(p, spikeCount, 1);
  drawRight(128, 63, text);

  if (!haveShot) {
    lcd.setFont(FONT_TEXT);
    p = putText(text, "Waiting for >");
    putAmpsShort(p, triggerUa());
    drawCentered(28, text);
    p = putText(text, "1s max ");
    putCurrent(p, max(secMaxUa, lastSecMaxUa));
    drawCentered(42, text);
    lcd.setFont(FONT_SMALL);
    lcd.drawStr(0, 63, cfg.autoArm ? "auto" : "single");
    return;
  }

  // Current on top; below it, when it was measured, the bus voltage during the same window
  const int top = 10, bottom = shotHasV ? 44 : 55, height = bottom - top;
  uint32_t scaleUa = niceCeil(max(shotPeakUa, triggerUa()));
  int32_t scaleRaw = max(uaToRaw(scaleUa), 1L);

  int trigY = bottom - min((int32_t)trigRaw, scaleRaw) * height / scaleRaw;
  for (int x = 0; x < 128; x += 3) lcd.drawPixel(x, trigY);
  for (int y = top; y <= bottom; y += 3) lcd.drawPixel(CAP_PRE, y);

  int lastY = 0;
  for (uint8_t i = 0; i < CAP_LEN; i++) {
    int y = bottom - min(absolute(shot[i]), scaleRaw) * height / scaleRaw;
    if (i) lcd.drawLine(i - 1, lastY, i, y);
    lastY = y;
  }

  if (shotHasV) {
    const int vTop = 47, vBottom = 55;
    int32_t vSpan = max((int32_t)(shotMaxMv - shotMinMv), 40L);   // 40 mV at least: no noise zoom
    for (uint8_t i = 0; i < CAP_LEN; i++) {
      int y = vBottom - (shotMv[i] - shotMinMv) * (vBottom - vTop) / vSpan;
      if (i) lcd.drawLine(i - 1, lastY, i, y);
      lastY = y;
    }
    putVoltsShort(text, shotMinMv);   // the lowest voltage in the window
    drawLabel(127, vBottom, text, true);
  }

  putAmpsShort(text, scaleUa);
  drawLabel(CAP_PRE + 3, top + 8, text, false);
  putDuration(text, (uint32_t)CAP_LEN * shotTickUs);
  drawLabel(127, top + 8, text, true);

  lcd.setFont(FONT_SMALL);
  p = putText(text, "pk ");
  p = putCurrent(p, shotPeakUa);
  p = putText(p, " w ");
  putDuration(p, shotWidthUs);
  lcd.drawStr(0, 63, text);
}

void totalRow(int baseline, const char *label, const char *value) {
  lcd.drawStr(0, baseline, label);
  drawRight(128, baseline, value);
}

void pageTotals(uint32_t now) {
  char text[20];
  drawHeader("TOTALS", counting ? "counting" : "idle", now);
  lcd.setFont(FONT_TEXT);

  putFixed(text, chargeUaMs / 360000000LL, 1);   // tenths of mAh
  totalRow(18, "mAh", text);

  putFixed(text, energyUwMs / 3600000000LL, 3);   // mWh, shown as Wh
  totalRow(27, "Wh", text);

  uint32_t seconds = countedUs / 1000000;
  char *p = putUint(text, seconds / 3600, 2);
  *p++ = ':';
  p = putUint(p, seconds / 60 % 60, 2);
  *p++ = ':';
  putUint(p, seconds % 60, 2);
  totalRow(36, "Time", text);

  if (countedUs >= 1000000) putCurrent(text, chargeUaMs * 1000 / (int64_t)countedUs);
  else strcpy(text, "--");
  totalRow(45, "Average", text);

  if (vMin <= vMax) {
    p = putFixed(text, roundDiv(vMin, 10), 2);
    *p++ = '-';
    putText(putFixed(p, roundDiv(vMax, 10), 2), "V");
  } else {
    strcpy(text, "--");
  }
  totalRow(54, "V load", text);

  putCurrent(text, peakUa);
  totalRow(63, "Peak", text);
}

// ---------------------------------------------------------------- messages

const char *toastText;
uint32_t toastEnd;

void toast(const char *text, uint32_t now) {
  toastText = text;
  toastEnd = now + 1200;
}

void drawToast(uint32_t now) {
  if (!toastText || due(now, toastEnd)) return;
  lcd.setFont(FONT_TEXT);
  int width = lcd.getStrWidth(toastText) + 12;
  int x = (128 - width) / 2;
  lcd.setDrawColor(0);
  lcd.drawBox(x - 1, 22, width + 2, 21);
  lcd.setDrawColor(1);
  lcd.drawRFrame(x, 23, width, 19, 3);
  lcd.drawStr(x + 6, 36, toastText);
}

// ---------------------------------------------------------------- actions

void actionZero(uint32_t now) {
  int32_t untared = shownRaw + cfg.tare;
  if (!sensorOk) {
    toast("No sensor", now);
  } else if (absolute(untared) > 50) {   // 0.5 mV: more than any offset, so a load is connected
    toast("Remove the load", now);
  } else {
    cfg.tare = untared;
    tareRaw = cfg.tare;
    toast("Zeroed", now);
  }
}

void actionResetTotals(uint32_t now) {
  resetTotals();
  toast("Totals reset", now);
}

void actionResetPeaks(uint32_t now) {
  resetPeaks();
  toast("Peaks reset", now);
}

void actionArm(uint32_t now) {
  captureArm();
  toast("Trigger armed", now);
}

void actionTimebase(uint32_t now) {
  cfg.timebase = (cfg.timebase + 1) % TIMEBASE_COUNT;
  clearGraph();
  toast(GRAPH_SPAN[cfg.timebase], now);
}

// Center held on a page
void quickAction(uint32_t now) {
  if (page == PAGE_METER) actionResetPeaks(now);
  else if (page == PAGE_GRAPH || page == PAGE_VOLTS) actionTimebase(now);
  else if (page == PAGE_SPIKE) actionArm(now);
  else actionResetTotals(now);
}

// ---------------------------------------------------------------- menu

enum MenuItem : uint8_t {
  M_BACK, M_ZERO, M_ARM, M_RESET_PEAKS, M_RESET_TOTALS,
  M_GRAPH, M_TRIGGER, M_TRIGMODE, M_RATE,
  M_ALERT_V, M_ALERT_I, M_ALERT_FLASH, M_ALERT_HOLD, M_AUTO_RESET,
  M_SHUNT, M_RANGE, M_CAL,
  M_VDEC, M_IDEC, M_CONTRAST, M_LIGHT, M_DIM, M_FLIP,
  M_COUNT
};

const char *const MENU_NAME[M_COUNT] = {
  "< Back", "Zero current", "Arm trigger", "Reset peaks", "Reset totals",
  "Graph span", "Trigger", "Trigger mode", "Sampling",
  "Low V alert", "High I alert", "Alert flash", "Alert hold", "Idle reset",
  "Shunt", "Range", "Current cal",
  "Volt digits", "Amp digits", "Contrast", "Backlight", "Dim after", "Display",
};

bool menuOpen, editing;
uint8_t menuSel, menuTop;
uint32_t menuIdleEnd;
#define MENU_ROWS 5
#define MENU_IDLE_MS 30000

bool isValueItem(uint8_t item) {
  return item >= M_GRAPH;
}

// The value shown at the right of a menu row; empty for actions
void menuValue(uint8_t item, char *text) {
  *text = 0;
  switch (item) {
    case M_GRAPH: strcpy(text, GRAPH_SPAN[cfg.timebase]); break;
    case M_TRIGGER: putAmpsShort(text, triggerUa()); break;
    case M_TRIGMODE: strcpy(text, cfg.autoArm ? "Auto" : "Single"); break;
    case M_RATE: strcpy(text, cfg.fast ? "0.5ms I" : "1ms V+I"); break;
    case M_ALERT_V:
      if (cfg.alertMv) putVoltsShort(text, cfg.alertMv);
      else strcpy(text, "Off");
      break;
    case M_ALERT_I:
      if (cfg.alertI) putAmpsShort(text, TRIGGER_MA[cfg.alertI - 1] * 1000L);
      else strcpy(text, "Off");
      break;
    case M_ALERT_FLASH: strcpy(text, FLASH_NAME[cfg.alertFlash]); break;
    case M_ALERT_HOLD: strcpy(text, cfg.alertLatch ? "Latch" : "2s"); break;
    case M_AUTO_RESET: strcpy(text, AUTO_RESET_NAME[cfg.autoReset]); break;
    case M_DIM: strcpy(text, DIM_NAME[cfg.dimIndex]); break;
    case M_SHUNT: strcpy(text, SHUNT_NAME[cfg.shunt]); break;
    case M_RANGE: putAmpsShort(text, (int64_t)(40000L << cfg.gain) * 1000000 / SHUNT_UOHM[cfg.shunt]); break;
    case M_CAL: {
      char *p = text;
      if (cfg.cal >= 0) *p++ = '+';
      putText(putFixed(p, cfg.cal, 1), "%");
      break;
    }
    case M_VDEC: putThousandths(text, 5000, cfg.vDecimals); strcat(text, "V"); break;
    case M_IDEC: putThousandths(text, 1000, cfg.iDecimals); strcat(text, "A"); break;
    case M_CONTRAST: putUint(text, cfg.contrast, 1); break;
    case M_LIGHT: putUint(text, cfg.backlight, 1); break;
    case M_FLIP: strcpy(text, ROTATION_NAME[cfg.rotation]); break;
  }
}

int stepClamp(int value, int dir, int low, int high) {
  return constrain(value + dir, low, high);
}

// Off, then 3.00 .. 5.50 V in 50 mV steps, then up to 20 V in 0.5 V steps
uint16_t stepAlertMv(uint16_t mv, int dir) {
  if (!mv) return dir > 0 ? 3000 : 0;
  int step = mv > 5500 || (mv == 5500 && dir > 0) ? 500 : 50;
  int next = mv + dir * step;
  if (next < 3000) return 0;
  return min(next, 20000);
}

void menuAdjust(uint8_t item, int dir) {
  switch (item) {
    case M_GRAPH:
      cfg.timebase = stepClamp(cfg.timebase, dir, 0, TIMEBASE_COUNT - 1);
      clearGraph();
      break;
    case M_TRIGGER:
      cfg.trigIndex = stepClamp(cfg.trigIndex, dir, 0, TRIGGER_COUNT - 1);
      applyTrigger();
      break;
    case M_TRIGMODE:
      cfg.autoArm = !cfg.autoArm;
      break;
    case M_RATE:
      cfg.fast = !cfg.fast;
      restartSensor();
      break;
    case M_ALERT_V:
      cfg.alertMv = stepAlertMv(cfg.alertMv, dir);
      alertBits &= ~ALERT_LOW_V;
      break;
    case M_ALERT_I:
      cfg.alertI = stepClamp(cfg.alertI, dir, 0, TRIGGER_COUNT);
      alertBits &= ~ALERT_HIGH_I;
      break;
    case M_ALERT_FLASH:
      cfg.alertFlash = (cfg.alertFlash + FLASH_OPTIONS + dir) % FLASH_OPTIONS;
      break;
    case M_ALERT_HOLD:
      cfg.alertLatch = !cfg.alertLatch;
      break;
    case M_AUTO_RESET:
      cfg.autoReset = stepClamp(cfg.autoReset, dir, 0, AUTO_RESET_OPTIONS - 1);
      break;
    case M_DIM:
      cfg.dimIndex = stepClamp(cfg.dimIndex, dir, 0, DIM_OPTIONS - 1);
      break;
    case M_SHUNT:
      cfg.shunt = (cfg.shunt + SHUNT_COUNT + dir) % SHUNT_COUNT;
      cfg.gain = SHUNT_DEFAULT_GAIN[cfg.shunt];
      cfg.cal = 0;
      cfg.tare = 0;
      updateScale();
      clearGraph();
      resetPeaks();
      restartSensor();
      break;
    case M_RANGE:
      cfg.gain = stepClamp(cfg.gain, dir, 0, 3);
      restartSensor();
      break;
    case M_CAL:
      cfg.cal = stepClamp(cfg.cal, dir, -200, 200);
      updateScale();
      applyTrigger();
      break;
    case M_VDEC:
      cfg.vDecimals = stepClamp(cfg.vDecimals, dir, 1, 3);
      break;
    case M_IDEC:
      cfg.iDecimals = stepClamp(cfg.iDecimals, dir, 1, 3);
      break;
    case M_CONTRAST:
      cfg.contrast = stepClamp(cfg.contrast, dir * 2, 0, 254);
      lcd.setContrast(cfg.contrast);
      break;
    case M_LIGHT:
      cfg.backlight = stepClamp(cfg.backlight, dir, 0, 10);   // lightUpdate() applies it
      break;
    case M_FLIP:
      cfg.rotation = (cfg.rotation + 4 + dir) % 4;
      lcd.setDisplayRotation(ROTATIONS[cfg.rotation]);
      break;
  }
}

void menuClose() {
  menuOpen = editing = false;
  settingsSave();
}

void menuSelect(uint32_t now) {
  if (isValueItem(menuSel)) {
    editing = !editing;
    if (!editing) settingsSave();
    return;
  }
  switch (menuSel) {
    case M_BACK: menuClose(); break;
    case M_ZERO: actionZero(now); menuClose(); break;
    case M_ARM: actionArm(now); menuClose(); page = PAGE_SPIKE; break;
    case M_RESET_PEAKS: actionResetPeaks(now); menuClose(); break;
    case M_RESET_TOTALS: actionResetTotals(now); menuClose(); break;
  }
}

void drawMenu(uint32_t now) {
  char text[16];
  lcd.drawBox(0, 0, 128, 9);
  lcd.setDrawColor(0);
  lcd.setFont(FONT_SMALL);
  lcd.drawStr(2, 7, editing ? "MENU   < change >" : "MENU");
  if (!editing) drawRight(126, 7, "hold = exit");
  lcd.setDrawColor(1);

  if (menuSel < menuTop) menuTop = menuSel;
  if (menuSel >= menuTop + MENU_ROWS) menuTop = menuSel - MENU_ROWS + 1;

  lcd.setFont(FONT_TEXT);
  for (uint8_t row = 0; row < MENU_ROWS; row++) {
    uint8_t item = menuTop + row;
    if (item >= M_COUNT) break;
    int y = 10 + row * 11;
    bool selected = item == menuSel;
    menuValue(item, text);

    if (selected && !editing) {
      lcd.drawBox(0, y, 124, 11);
      lcd.setDrawColor(0);
    }
    lcd.drawStr(2, y + 9, MENU_NAME[item]);
    if (*text) {
      if (selected && editing) {
        int width = lcd.getStrWidth(text);
        lcd.drawFrame(0, y, 124, 11);
        lcd.drawBox(121 - width - 3, y + 1, width + 4, 9);
        lcd.setDrawColor(0);
      }
      drawRight(121, y + 9, text);
    }
    lcd.setDrawColor(1);
  }

  // Scroll bar
  int barH = 54 * MENU_ROWS / M_COUNT;
  int barY = 10 + (54 - barH) * menuTop / (M_COUNT - MENU_ROWS);
  lcd.drawVLine(126, 10, 54);
  lcd.drawBox(125, barY, 3, barH);
}

// ---------------------------------------------------------------- buttons

enum Event : uint8_t { EV_NONE, EV_LEFT, EV_RIGHT, EV_CLICK, EV_HOLD };

#define DEBOUNCE_MS     25
#define HOLD_MS         700
#define REPEAT_DELAY_MS 400
#define REPEAT_MS       90

struct Button {
  uint32_t pin;
  bool down, held;
  uint32_t changed, nextRepeat;
};

Button buttons[3] = {{PIN_LEFT}, {PIN_CENTER}, {PIN_RIGHT}};

// Left and right fire on press and repeat while held (in the menu). Center: click on release,
// or hold.
Event pollButtons(uint32_t now) {
  for (uint8_t i = 0; i < 3; i++) {
    Button &b = buttons[i];
    bool pressed = digitalRead(b.pin) == LOW;
    Event direction = i == 0 ? EV_LEFT : EV_RIGHT;

    if (pressed != b.down && now - b.changed >= DEBOUNCE_MS) {
      b.down = pressed;
      b.changed = now;
      if (pressed) {
        b.held = false;
        b.nextRepeat = now + REPEAT_DELAY_MS;
        if (i != 1) return direction;
      } else if (i == 1 && !b.held) {
        return EV_CLICK;
      }
    }

    if (b.down) {
      if (i == 1) {
        if (!b.held && now - b.changed >= HOLD_MS) {
          b.held = true;
          return EV_HOLD;
        }
      } else if (menuOpen && due(now, b.nextRepeat)) {
        b.nextRepeat = now + REPEAT_MS;
        return direction;
      }
    }
  }
  return EV_NONE;
}

bool dimmed(uint32_t now) {
  return cfg.dimIndex && now - lastPressMs >= DIM_S[cfg.dimIndex] * 1000UL;
}

void handleEvent(Event ev, uint32_t now) {
  if (ev == EV_NONE) {
    if (menuOpen && due(now, menuIdleEnd)) menuClose();
    return;
  }

  bool wasDimmed = dimmed(now);
  lastPressMs = now;
  if (alertBits && cfg.alertLatch) {   // the first press acknowledges a latched alert
    alertBits = 0;
    return;
  }
  if (wasDimmed) return;   // and the first press after dimming only wakes the light

  if (!menuOpen) {
    if (ev == EV_LEFT) page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
    else if (ev == EV_RIGHT) page = (page + 1) % PAGE_COUNT;
    else if (ev == EV_HOLD) quickAction(now);
    else if (ev == EV_CLICK) {
      menuOpen = true;
      editing = false;
      menuSel = menuTop = 0;
      menuIdleEnd = now + MENU_IDLE_MS;
    }
    return;
  }

  menuIdleEnd = now + MENU_IDLE_MS;
  int dir = ev == EV_LEFT ? -1 : 1;
  if (ev == EV_HOLD) menuClose();
  else if (ev == EV_CLICK) menuSelect(now);
  else if (editing) menuAdjust(menuSel, dir);
  else menuSel = (menuSel + M_COUNT + dir) % M_COUNT;
}

// ---------------------------------------------------------------- backlight, power, page memory

uint8_t lightLevel = 0xFF;   // what the backlight is set to; 0xFF forces the next update

void lightUpdate(uint32_t now) {
  uint8_t want = cfg.backlight;
  if (dimmed(now) && want > DIM_LEVEL) want = DIM_LEVEL;
  bool event = !due(now, overloadUntil) || !due(now, flashUntil);
  bool flash = (cfg.alertFlash >= 1 && alertBits) || (cfg.alertFlash == 2 && event);
  if (flash) want = (now / FLASH_HALF_MS) & 1 ? 10 : 0;
  if (powerLow) want = 0;   // the supply is going away: the charge left is for saving
  if (want != lightLevel) {
    lightLevel = want;
    setBacklight(want);
  }
}

// The interrupt flags a voltage drop below POWER_FAIL_MV (and has already cut the light).
// Saving a record without an erase takes well under 1 ms; how long the 3.3 V rail lasts
// depends on the capacitors, so this is a best effort.
void powerWatch(uint32_t now) {
  if (powerFail) {
    powerFail = false;
    powerLow = true;
    powerFailAt = now;
    if (chargeUaMs / 1000 != savedChargeUaS) saveTotals();
  }
  uint16_t mv = busMv;
  if (powerLow && mv >= POWER_OK_MV) {
    powerLow = false;   // only a dip: lightUpdate() turns the light back on
    lightLevel = 0xFF;
  }
  if (!powerArmed && !powerLow && sensorOk && !fastMode && mv >= POWER_OK_MV &&
      due(now, powerFailAt + POWER_REARM_MS)) {
    powerArmed = true;
  }
}

// The page shown at power-up is the last one looked at for a while: few flash writes
void pageMemory(uint32_t now) {
  if (page != lastPage) {
    lastPage = page;
    pageSaveAt = now + PAGE_SAVE_MS;
  }
  if (page != cfg.page && !menuOpen && !powerLow && due(now, pageSaveAt)) {
    cfg.page = page;
    settingsSave();
  }
}

// ---------------------------------------------------------------- main

void splash() {
  for (uint8_t n = 1; n <= 10; n++) {
    lcd.clearBuffer();
    lcd.setFont(FONT_TEXT);
    drawCentered(22, "USB POWER METER");
    lcd.setFont(FONT_SMALL);
    drawCentered(36, "INA219 / STM32F051");
    lcd.drawFrame(24, 46, 80, 6);
    lcd.drawBox(26, 48, n * 76 / 10, 2);
    lcd.sendBuffer();
    setBacklight((uint32_t)cfg.backlight * n / 10);
    delay(50);
  }
}

void setup() {
  bool watchdogReset = resetByWatchdog();
  pinMode(PIN_LCD_LED, INPUT);   // backlight off until the PWM is set up
  for (uint8_t i = 0; i < 3; i++) pinMode(buttons[i].pin, INPUT_PULLUP);

  settingsLoad();
  updateScale();
  page = lastPage = cfg.page;

  lcd.begin();
  lcd.setDisplayRotation(ROTATIONS[cfg.rotation]);
  lcd.setContrast(cfg.contrast);
  lcd.setFontMode(1);
  splash();
  watchdogStart();

  if (inaConnect()) restartSensor();
  else nextReconnect = millis() + 1000;

  loadTotals();
  clearGraph();
  nextSave = millis() + SAVE_PERIOD_MS;
  lastPressMs = millis();
  if (watchdogReset) toast("Watchdog reset", millis());
}

void loop() {
  static uint32_t nextFrame, nextCollect;
  uint32_t now = millis();
  watchdogKick();
  powerWatch(now);

  if (sensorLost) {
    if (sensorOk) flashUntil = now + 1500;   // the sensor just stopped answering
    sensorOk = false;
    if (due(now, nextReconnect)) {
      samplerStop();
      if (inaConnect()) restartSensor();
      nextReconnect = now + 1000;
    }
  }

  if (due(now, nextCollect)) {
    nextCollect = now + COLLECT_MS;
    collect(now);
    updateDerived(now);
  }

  handleEvent(pollButtons(now), now);
  pageMemory(now);
  lightUpdate(now);

  if (!due(now, nextFrame)) return;
  nextFrame = now + FRAME_MS;
  blinkOn = (now / 400) & 1;

  lcd.clearBuffer();
  if (menuOpen) drawMenu(now);
  else if (page == PAGE_METER) pageMeter(now);
  else if (page == PAGE_GRAPH) pageGraph(now);
  else if (page == PAGE_VOLTS) pageVolts(now);
  else if (page == PAGE_SPIKE) pageSpike(now);
  else pageTotals(now);
  drawToast(now);
  lcd.sendBuffer();
}
