// Pins and clock for the bare STM32F051K8T6 (LQFP32).
//
// LCD ST7567 128x64 (hardware SPI1):
//   CS PA4, DC PB0, RST PB1, SCK PA5, SDA PA7
//   LED PA11: backlight cathode, active LOW, no series resistor (duty capped by BL_MAX_PWM)
// INA219 (I2C1, 4.7k pull-ups to 3.3 V recommended): SCL PB6, SDA PB7, A0 and A1 to GND
// Joystick, each button to GND: left PA0, center PA1, right PA2
// ST-Link: SWDIO PA13, SWCLK PA14, GND, 3.3 V (and NRST if connecting fails)
#pragma once

#include <Arduino.h>

#define PIN_LCD_CS  PA4
#define PIN_LCD_DC  PB0
#define PIN_LCD_RST PB1
#define PIN_LCD_LED PA11   // must be a pin with a hardware PWM channel (TIM1_CH4)

#define PIN_LEFT   PA0
#define PIN_CENTER PA1
#define PIN_RIGHT  PA2

#ifndef BL_MAX_PWM
#define BL_MAX_PWM 64
#endif

// The variant leaves the clock at the 8 MHz reset default. HSI/2 * 12 = 48 MHz, no crystal.
extern "C" void SystemClock_Config(void) {
  RCC_OscInitTypeDef osc = {};
  RCC_ClkInitTypeDef clk = {};

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;   // HSI / 2 on this part
  osc.PLL.PLLMUL = RCC_PLL_MUL12;
  osc.PLL.PREDIV = RCC_PREDIV_DIV1;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1) != HAL_OK) Error_Handler();
}

// True once the time t has been reached; safe across millis() wraparound
inline bool due(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

inline int32_t absolute(int32_t v) {
  return v < 0 ? -v : v;
}

// Backlight level 0..10, squared for the eye. The pin is the LED cathode: low duty = light.
// Open-drain, so wired the other way round the LED stays dark instead of being overdriven.
void setBacklight(uint8_t level) {
  if (level > 10) level = 10;
  uint32_t pwm = (uint32_t)level * level * BL_MAX_PWM / 100;
  analogWrite(PIN_LCD_LED, 255 - pwm);
  GPIOA->OTYPER |= GPIO_OTYPER_OT_11;   // analogWrite configures push-pull: force open-drain
  GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODER11) | GPIO_MODER_MODER11_1;   // back on the timer
}

// Backlight off at once, from anywhere (also an interrupt): the pin becomes an input, so the LED
// cathode floats. The next setBacklight() hands the pin back to the timer.
inline void backlightCut() {
  GPIOA->MODER &= ~GPIO_MODER_MODER11;
}

// ---------------------------------------------------------------- watchdog

// Independent watchdog on the ~40 kHz LSI: 40 kHz / 32 = 1250 Hz, 2500 counts = about 2 s.
// The longest blocking step (a sensor rescan with a stuck bus) takes a few hundred ms.
void watchdogStart() {
  RCC->APB2ENR |= RCC_APB2ENR_DBGMCUEN;
  DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;   // no resets while halted in a debugger
  IWDG->KR = 0xCCCC;   // start
  IWDG->KR = 0x5555;   // unlock PR and RLR
  IWDG->PR = 3;        // divide by 32
  IWDG->RLR = 2500;
  for (uint32_t n = 0; IWDG->SR && n < 100000; n++) {}
  IWDG->KR = 0xAAAA;
}

inline void watchdogKick() {
  IWDG->KR = 0xAAAA;
}

// True if the last reset came from the watchdog. Clears the reset flags.
bool resetByWatchdog() {
  bool watchdog = RCC->CSR & RCC_CSR_IWDGRSTF;
  RCC->CSR |= RCC_CSR_RMVF;
  return watchdog;
}
