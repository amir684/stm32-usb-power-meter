// INA219 sampling engine.
//
// A timer interrupt reads the shunt register every 1 ms (or every 0.5 ms in fast mode), and the bus
// voltage with it (every 16th sample at 100 kHz, never in fast mode), so short
// current spikes are caught even while the main loop is busy drawing. Everything else reads the
// results the interrupt leaves behind. The I2C driver talks to the I2C1 registers directly and
// never waits on interrupts, so it is safe to call from inside the timer interrupt.
//
// Rule: the main loop may only use I2C while the sampler is stopped (samplerStop()).
#pragma once

#include "hw.h"

// ---------------------------------------------------------------- I2C1, polled

#define I2C_TIMING_400K 0x00310309UL   // I2C clock = HSI 8 MHz, values from RM0091
#define I2C_TIMING_100K 0x10420F13UL
#define I2C_TIMEOUT     20000UL        // polling loops, a few ms

#define SCL_BIT 6   // PB6
#define SDA_BIT 7   // PB7

// A software reset clears every flag and releases both lines
static void i2cRecover() {
  I2C1->CR1 &= ~I2C_CR1_PE;
  for (volatile uint8_t i = 0; i < 4; i++) {}   // PE must stay low for at least 3 APB clocks
  I2C1->CR1 |= I2C_CR1_PE;
}

static bool i2cWaitFor(uint32_t flag) {
  for (uint32_t n = 0; n < I2C_TIMEOUT; n++) {
    uint32_t isr = I2C1->ISR;
    if (isr & flag) return true;
    if (isr & (I2C_ISR_NACKF | I2C_ISR_BERR | I2C_ISR_ARLO)) break;
  }
  i2cRecover();
  return false;
}

void i2cInit(uint32_t timing) {
  RCC->AHBENR |= RCC_AHBENR_GPIOBEN;
  RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
  RCC->CFGR3 &= ~RCC_CFGR3_I2C1SW;   // I2C clock = HSI 8 MHz, independent of the PLL
  I2C1->CR1 = 0;

  GPIOB->OTYPER |= (1UL << SCL_BIT) | (1UL << SDA_BIT);                  // open-drain
  GPIOB->PUPDR = (GPIOB->PUPDR & ~(0xFUL << 12)) | (0x5UL << 12);        // weak pull-ups as a backup
  GPIOB->OSPEEDR |= 0xFUL << 12;

  // Nine clocks with SDA released free a slave that a reset left in the middle of a byte
  GPIOB->BSRR = (1UL << SCL_BIT) | (1UL << SDA_BIT);
  GPIOB->MODER = (GPIOB->MODER & ~(0xFUL << 12)) | (0x5UL << 12);        // both GPIO outputs
  for (uint8_t i = 0; i < 9; i++) {
    GPIOB->BRR = 1UL << SCL_BIT;
    delayMicroseconds(5);
    GPIOB->BSRR = 1UL << SCL_BIT;
    delayMicroseconds(5);
  }
  GPIOB->BRR = 1UL << SDA_BIT;   // STOP: SDA rises while SCL is high
  delayMicroseconds(5);
  GPIOB->BSRR = 1UL << SDA_BIT;
  delayMicroseconds(5);

  GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFFUL << 24)) | (0x11UL << 24);    // AF1 = I2C1
  GPIOB->MODER = (GPIOB->MODER & ~(0xFUL << 12)) | (0xAUL << 12);        // alternate function

  I2C1->TIMINGR = timing;
  I2C1->CR1 = I2C_CR1_PE;
}

static bool i2cWrite(uint8_t addr, const uint8_t *data, uint8_t length) {
  I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
  I2C1->CR2 = (addr << 1) | ((uint32_t)length << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
  for (uint8_t i = 0; i < length; i++) {
    if (!i2cWaitFor(I2C_ISR_TXIS)) return false;
    I2C1->TXDR = data[i];
  }
  if (!i2cWaitFor(I2C_ISR_STOPF)) return false;
  I2C1->ICR = I2C_ICR_STOPCF;
  return true;
}

static bool i2cRead16(uint8_t addr, uint16_t &value) {
  I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
  I2C1->CR2 = (addr << 1) | I2C_CR2_RD_WRN | (2UL << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
  if (!i2cWaitFor(I2C_ISR_RXNE)) return false;
  uint16_t high = I2C1->RXDR;
  if (!i2cWaitFor(I2C_ISR_RXNE)) return false;
  value = (high << 8) | I2C1->RXDR;
  if (!i2cWaitFor(I2C_ISR_STOPF)) return false;
  I2C1->ICR = I2C_ICR_STOPCF;
  return true;
}

// ---------------------------------------------------------------- INA219 registers

#define REG_CONFIG 0
#define REG_SHUNT  1
#define REG_BUS    2

#define CFG_BRNG_32V        (1U << 13)
#define CFG_PGA(g)          ((uint16_t)(g) << 11)   // 0..3 = +-40, 80, 160, 320 mV
#define CFG_BADC_12BIT      (0x3U << 7)             // 532 us, no averaging
#define CFG_SADC_12BIT      (0x3U << 3)             // 532 us, no averaging
#define CFG_MODE_SHUNT_CONT 5U
#define CFG_MODE_BOTH_CONT  7U

#define RAW_FULL_SCALE 4000   // shunt counts (10 uV each) at PGA /1

uint8_t inaAddress = 0x40;
bool i2cFast;   // the sensor answered at 400 kHz

bool inaWrite(uint8_t reg, uint16_t value) {
  uint8_t bytes[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
  return i2cWrite(inaAddress, bytes, 3);
}

// Sets the register pointer; following reads return that register without rewriting it
bool inaPoint(uint8_t reg) {
  return i2cWrite(inaAddress, &reg, 1);
}

bool inaReadNext(uint16_t &value) {
  return i2cRead16(inaAddress, value);
}

bool inaRead(uint8_t reg, uint16_t &value) {
  return inaPoint(reg) && inaReadNext(value);
}

// Tries 400 kHz first, then 100 kHz for weak pull-ups, on all 16 addresses the A0/A1 pins allow
bool inaConnect() {
  static const uint32_t TIMINGS[2] = {I2C_TIMING_400K, I2C_TIMING_100K};
  for (uint8_t s = 0; s < 2; s++) {
    i2cInit(TIMINGS[s]);
    i2cFast = s == 0;
    for (inaAddress = 0x40; inaAddress < 0x50; inaAddress++) {
      uint16_t value;
      if (inaRead(REG_CONFIG, value)) return true;
    }
  }
  inaAddress = 0x40;
  return false;
}

// ---------------------------------------------------------------- sampler (timer interrupt)

#define CAP_LEN   128   // capture length in samples: one per screen column. Power of two.
#define CAP_PRE   16    // samples kept from before the trigger
#define BUS_EVERY 16    // at 100 kHz the bus voltage is read only every 16th sample (about 16 ms)

// Power loss: below POWER_FAIL_MV the interrupt cuts the backlight and flags it for the main loop,
// which saves the totals. It re-arms once the voltage is back above POWER_OK_MV.
#define POWER_FAIL_MV 4400
#define POWER_OK_MV   4600

enum CaptureState : uint8_t { CAP_IDLE, CAP_ARMED, CAP_CAPTURING, CAP_DONE };

HardwareTimer *sampleTimer;
uint16_t tickUs = 1000;   // sample period
bool fastMode;            // shunt only: twice the rate, the bus voltage is held
uint8_t busEvery = BUS_EVERY;

// Written by the interrupt, collected and cleared by the main loop (see samplerCollect)
volatile int32_t winSum;
volatile uint16_t winCount;
volatile int16_t winLow = INT16_MAX, winHigh = INT16_MIN;
volatile uint16_t busMv;
volatile uint16_t winMvLow = 0xFFFF, winMvHigh;   // bus voltage readings in this window
volatile bool powerArmed, powerFail;
volatile bool overloadSeen;
volatile bool sensorLost = true;
volatile uint8_t i2cErrors;
volatile uint32_t spikeCount;

// Set by the main loop
volatile int16_t tareRaw;
volatile int16_t trigRaw = INT16_MAX, trigLowRaw = INT16_MAX;   // trigger level, and the level that ends a spike
int16_t overloadRaw = INT16_MAX;

// Spike capture: a ring buffer while armed, frozen once CAP_DONE
volatile int16_t cap[CAP_LEN];
volatile uint16_t capMv[CAP_LEN];   // the bus voltage at each captured sample (held at 0.5 ms)
volatile uint8_t capState, capPos, capFill, capPost, capStart;
volatile bool aboveTrigger;
uint8_t busDivider;

static void sampleError() {
  if (++i2cErrors >= 3) sensorLost = true;
}

void onSampleTick() {
  if (sensorLost) return;

  uint16_t reg;
  if (!inaReadNext(reg)) {
    sampleError();
    return;
  }
  i2cErrors = 0;

  int32_t raw = (int16_t)reg;
  if (absolute(raw) >= overloadRaw) overloadSeen = true;
  raw -= tareRaw;
  raw = constrain(raw, -32767L, 32767L);

  winSum += raw;
  winCount++;
  if (raw < winLow) winLow = raw;
  if (raw > winHigh) winHigh = raw;

  // Rising edge through the trigger level, with hysteresis, counts one spike
  int32_t magnitude = absolute(raw);
  bool rising = false;
  if (!aboveTrigger && magnitude >= trigRaw) {
    aboveTrigger = rising = true;
    spikeCount++;
  } else if (aboveTrigger && magnitude < trigLowRaw) {
    aboveTrigger = false;
  }

  // Bus voltage: about 0.25 ms of I2C at 400 kHz, so every sample fits in the 1 ms tick
  if (!fastMode && ++busDivider >= busEvery) {
    busDivider = 0;
    uint16_t bus;
    if (inaRead(REG_BUS, bus)) {
      uint16_t mv = (bus >> 3) * 4;
      busMv = mv;
      if (mv < winMvLow) winMvLow = mv;
      if (mv > winMvHigh) winMvHigh = mv;
      if (powerArmed && mv < POWER_FAIL_MV) {
        backlightCut();   // the light is most of the load: leave the charge for saving
        powerArmed = false;
        powerFail = true;
      }
    }
    if (!inaPoint(REG_SHUNT)) sampleError();
  }

  uint8_t state = capState;
  if (state == CAP_ARMED || state == CAP_CAPTURING) {
    cap[capPos] = raw;
    capMv[capPos] = busMv;
    capPos = (capPos + 1) & (CAP_LEN - 1);
    if (state == CAP_ARMED) {
      if (capFill < CAP_PRE) {
        capFill++;
      } else if (rising) {
        capPost = CAP_LEN - 1 - CAP_PRE;
        capState = CAP_CAPTURING;
      }
    } else if (--capPost == 0) {
      capStart = capPos;   // the oldest sample: CAP_PRE before the trigger
      capState = CAP_DONE;
    }
  }

}

void samplerStop() {
  if (sampleTimer) sampleTimer->pause();
}

// Configures the INA219 for gain 0..3 and starts the interrupt. False if the sensor did not answer.
bool samplerStart(uint8_t gain, bool fast) {
  samplerStop();
  fastMode = fast;
  tickUs = fast ? 500 : 1000;   // a little faster than the 532 us / 1064 us conversions: none is missed
  overloadRaw = (RAW_FULL_SCALE << gain) * 98 / 100;
  busEvery = i2cFast ? 1 : BUS_EVERY;

  // Both channels first, so a bus voltage exists even in shunt-only mode, where it is held
  const uint16_t both = CFG_BRNG_32V | CFG_PGA(gain) | CFG_BADC_12BIT | CFG_SADC_12BIT | CFG_MODE_BOTH_CONT;
  uint16_t bus = 0;
  bool ok = inaWrite(REG_CONFIG, both);
  delay(3);
  ok = ok && inaRead(REG_BUS, bus);
  if (ok && fast) ok = inaWrite(REG_CONFIG, CFG_BRNG_32V | CFG_PGA(gain) | CFG_SADC_12BIT | CFG_MODE_SHUNT_CONT);
  ok = ok && inaPoint(REG_SHUNT);

  busMv = (bus >> 3) * 4;
  i2cErrors = 0;
  busDivider = 0;
  sensorLost = !ok;
  if (!ok) return false;

  if (!sampleTimer) {
    sampleTimer = new HardwareTimer(TIM3);
    sampleTimer->attachInterrupt(onSampleTick);
  }
  sampleTimer->setOverflow(tickUs, MICROSEC_FORMAT);
  sampleTimer->refresh();
  sampleTimer->resume();
  return true;
}

struct SampleWindow {
  int32_t sum;
  uint16_t count;
  int16_t low, high;
  uint16_t mv;               // the latest bus voltage
  uint16_t mvLow, mvHigh;    // the readings in this window; mvLow > mvHigh if there were none
  bool overload;
};

// Everything the interrupt gathered since the last call
SampleWindow samplerCollect() {
  SampleWindow w;
  noInterrupts();
  w.sum = winSum;
  w.count = winCount;
  w.low = winLow;
  w.high = winHigh;
  w.mv = busMv;
  w.mvLow = winMvLow;
  w.mvHigh = winMvHigh;
  winMvLow = 0xFFFF;
  winMvHigh = 0;
  w.overload = overloadSeen;
  winSum = 0;
  winCount = 0;
  winLow = INT16_MAX;
  winHigh = INT16_MIN;
  overloadSeen = false;
  interrupts();
  return w;
}

void captureArm() {
  noInterrupts();
  capFill = 0;
  capState = CAP_ARMED;
  interrupts();
}

void setTrigger(int32_t raw) {
  raw = constrain(raw, 1L, 32000L);
  noInterrupts();
  trigRaw = raw;
  trigLowRaw = raw - raw / 8;
  aboveTrigger = false;
  interrupts();
}
