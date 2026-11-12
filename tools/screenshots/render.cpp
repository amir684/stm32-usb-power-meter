// Draws every page of the meter with the real drawing code from src/main.cpp (compiled on a PC
// against the U8g2 C library) and sample data, and writes each frame as a 128x64 PBM image.
// Run through make.py, which builds this and turns the frames into PNGs.
#include <math.h>
#include <stdint.h>
#include <stdio.h>

uint32_t simMillis = 100000;

#include "main.cpp"

static const char *outDir = ".";

static void save(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.pbm", outDir, name);
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(1);
  }
  fprintf(f, "P1\n128 64\n");
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 128; x++) fputc(lcd.pixel(x, y) ? '1' : '0', f);
    fputc('\n', f);
  }
  fclose(f);
  printf("  %s\n", path);
}

// Repeatable noise in -1..1
static double noise() {
  static uint32_t state = 12345;
  state = state * 1103515245 + 12345;
  return ((state >> 16) & 0x7FFF) / 16383.5 - 1.0;
}

// A phone charging: idle, then about 1.45 A falling off slowly, then tapering at the end.
// Voltage: the cable and hub drop about 0.1 V per amp.
static void fillHistory() {
  graphFill = GRAPH_W;
  graphHead = 0;
  for (int k = 0; k < GRAPH_W; k++) {
    double ma;
    if (k < 26) ma = 95 + 10 * noise();
    else if (k < 104) ma = 1480 - (k - 26) * 2.2 + 35 * noise();
    else ma = 1300 - (k - 104) * 17 + 30 * noise();
    graphAvg[k] = (int16_t)ma;
    graphPeak[k] = (int16_t)(ma * (1.25 + 0.2 * fabs(noise())));
    double mv = 5090 - ma * 0.1 + 4 * noise();
    graphVAvg[k] = (uint16_t)mv;
    graphVLow[k] = (uint16_t)(mv - 15 - ma * 0.05 * fabs(noise()));
  }
  shownUa = (int32_t)(graphAvg[GRAPH_W - 1] * 1000L);
  shownMv = graphVAvg[GRAPH_W - 1];
  peakUa = 2134000;
}

// A device plugging in: 160 mA, a jump to 1.85 A that dies away in about 15 ms
static void fillSpike() {
  shotMinMv = 0xFFFF;
  shotMaxMv = 0;
  int32_t peakRaw = 0;
  for (int i = 0; i < CAP_LEN; i++) {
    double ma = i < CAP_PRE ? 160 : 160 + 1690 * exp(-(i - CAP_PRE) / 6.0);
    ma += 12 * noise();
    shot[i] = (int16_t)ma;
    shotMv[i] = (uint16_t)(5060 - (ma - 160) * 0.13 + 3 * noise());
    shotMinMv = min(shotMinMv, shotMv[i]);
    shotMaxMv = max(shotMaxMv, shotMv[i]);
    peakRaw = max(peakRaw, absolute(shot[i]));
  }
  uint8_t n = CAP_PRE;
  while (n < CAP_LEN && absolute(shot[n]) >= trigLowRaw) n++;
  shotTickUs = 1000;
  shotWidthUs = (uint32_t)(n - CAP_PRE) * shotTickUs;
  shotPeakUa = rawToUa(peakRaw);
  shotHasV = true;
  haveShot = true;
  capState = CAP_ARMED;
  spikeCount = 7;
}

static void frame(uint8_t which, const char *name) {
  page = which;
  lcd.clearBuffer();
  if (menuOpen) drawMenu(simMillis);
  else if (page == PAGE_METER) pageMeter(simMillis);
  else if (page == PAGE_GRAPH) pageGraph(simMillis);
  else if (page == PAGE_VOLTS) pageVolts(simMillis);
  else if (page == PAGE_SPIKE) pageSpike(simMillis);
  else pageTotals(simMillis);
  save(name);
}

int main(int argc, char **argv) {
  if (argc > 1) outDir = argv[1];

  settingsDefaults();
  cfg.rotation = 0;   // the picture as it is seen, whatever the mounting
  updateScale();
  applyTrigger();
  lcd.setFontMode(1);
  sensorOk = true;
  blinkOn = true;
  cfg.timebase = 2;

  fillHistory();
  fillSpike();

  frame(PAGE_METER, "meter");
  frame(PAGE_GRAPH, "graph");
  frame(PAGE_VOLTS, "volts");
  frame(PAGE_SPIKE, "spike");

  chargeUaMs = 12345LL * 360000000LL;      // 1234.5 mAh
  energyUwMs = 6210LL * 3600000000LL;      // 6.210 Wh
  countedUs = (2 * 3600 + 27 * 60 + 13) * 1000000ULL;
  vMin = 4870;
  vMax = 5085;
  counting = true;
  frame(PAGE_TOTALS, "totals");

  // A low-voltage alert on the meter page
  cfg.alertMv = 4750;
  alertBits = ALERT_LOW_V;
  shownMv = 4712;
  shownUa = 1902000;
  frame(PAGE_METER, "alert");
  alertBits = 0;

  // The menu, editing the low-voltage alert
  cfg.alertI = 12;   // 2 A
  menuOpen = true;
  editing = true;
  menuSel = M_ALERT_V;
  menuTop = M_RATE;
  frame(PAGE_METER, "menu");
  return 0;
}
