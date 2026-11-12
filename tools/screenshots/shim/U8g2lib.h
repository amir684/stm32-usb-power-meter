// A thin C++ wrapper over the U8g2 C library with the calls main.cpp makes. The display is set up
// with the real ST7567 JLX12864 full-buffer driver, but nothing is sent anywhere: the screenshot
// is read straight from the frame buffer.
#pragma once

#include "Arduino.h"
#include "u8g2.h"

class U8G2 {
 public:
  u8g2_t u8g2;

  void begin() {}
  void sendBuffer() {}
  void clearBuffer() { u8g2_ClearBuffer(&u8g2); }
  void setContrast(uint8_t) {}
  void setDisplayRotation(const u8g2_cb_t *r) { u8g2_SetDisplayRotation(&u8g2, r); }
  void setFontMode(uint8_t mode) { u8g2_SetFontMode(&u8g2, mode); }
  void setFont(const uint8_t *font) { u8g2_SetFont(&u8g2, font); }
  void setDrawColor(uint8_t color) { u8g2_SetDrawColor(&u8g2, color); }
  int drawStr(int x, int y, const char *s) { return u8g2_DrawStr(&u8g2, x, y, s); }
  int getStrWidth(const char *s) { return u8g2_GetStrWidth(&u8g2, s); }
  void drawPixel(int x, int y) { u8g2_DrawPixel(&u8g2, x, y); }
  void drawHLine(int x, int y, int w) { u8g2_DrawHLine(&u8g2, x, y, w); }
  void drawVLine(int x, int y, int h) { u8g2_DrawVLine(&u8g2, x, y, h); }
  void drawLine(int x1, int y1, int x2, int y2) { u8g2_DrawLine(&u8g2, x1, y1, x2, y2); }
  void drawBox(int x, int y, int w, int h) { u8g2_DrawBox(&u8g2, x, y, w, h); }
  void drawFrame(int x, int y, int w, int h) { u8g2_DrawFrame(&u8g2, x, y, w, h); }
  void drawRFrame(int x, int y, int w, int h, int r) { u8g2_DrawRFrame(&u8g2, x, y, w, h, r); }
  void drawDisc(int x, int y, int r) { u8g2_DrawDisc(&u8g2, x, y, r, U8G2_DRAW_ALL); }

  // 1 if the pixel at (x, y) is set
  int pixel(int x, int y) {
    const uint8_t *buffer = u8g2_GetBufferPtr(&u8g2);
    return (buffer[(y / 8) * 128 + x] >> (y % 8)) & 1;
  }
};

class U8G2_ST7567_JLX12864_F_4W_HW_SPI : public U8G2 {
 public:
  U8G2_ST7567_JLX12864_F_4W_HW_SPI(const u8g2_cb_t *rotation, int, int, int) {
    u8g2_Setup_st7567_jlx12864_f(&u8g2, rotation, u8x8_byte_empty, u8x8_dummy_cb);
  }
};
