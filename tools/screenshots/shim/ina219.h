// PC stand-in for include/ina219.h: the state main.cpp reads, filled in by render.cpp instead
// of a timer interrupt
#pragma once

#include "hw.h"

#define CAP_LEN 128
#define CAP_PRE 16
#define POWER_FAIL_MV 4400
#define POWER_OK_MV   4600

enum CaptureState : uint8_t { CAP_IDLE, CAP_ARMED, CAP_CAPTURING, CAP_DONE };

uint16_t tickUs = 1000;
bool fastMode;
volatile uint16_t busMv;
volatile bool powerArmed, powerFail;
volatile bool sensorLost;
volatile uint32_t spikeCount;
volatile int16_t tareRaw;
volatile int16_t trigRaw = INT16_MAX, trigLowRaw = INT16_MAX;
volatile int16_t cap[CAP_LEN];
volatile uint16_t capMv[CAP_LEN];
volatile uint8_t capState, capStart;

struct SampleWindow {
  int32_t sum;
  uint16_t count;
  int16_t low, high;
  uint16_t mv;
  uint16_t mvLow, mvHigh;
  bool overload;
};

inline SampleWindow samplerCollect() {
  SampleWindow w = {};
  w.mvLow = 0xFFFF;
  return w;
}
inline void samplerStop() {}
inline bool samplerStart(uint8_t, bool) { return true; }
inline bool inaConnect() { return true; }
inline void captureArm() { capState = CAP_ARMED; }
inline void setTrigger(int32_t raw) {
  trigRaw = raw;
  trigLowRaw = raw - raw / 8;
}
