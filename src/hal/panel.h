// Panel HAL for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// 360x360 round IPS driven by an ST77916 over QSPI (four data lines, not
// plain SPI) — the reason this board cannot use TFT_eSPI at all.
//
// The bus and panel are file-scope globals rather than heap objects because
// their constructors only latch pin numbers; no hardware is touched until
// begin(). That lets main.cpp declare the canvas as a global too, so the
// original firmware's "createSprite() in setup()" flow carries over
// unchanged.
#pragma once

#include <Arduino_GFX_Library.h>
#include "st77916_waveshare.h"

// Display — ST77916 QSPI
static const int8_t PIN_LCD_CS  = 14;
static const int8_t PIN_LCD_CLK = 13;
static const int8_t PIN_LCD_D0  = 15;
static const int8_t PIN_LCD_D1  = 16;
static const int8_t PIN_LCD_D2  = 17;
static const int8_t PIN_LCD_D3  = 18;
static const int8_t PIN_LCD_RST = 21;
static const int8_t PIN_LCD_BL  = 47;

static const int16_t PANEL_W = 360;
static const int16_t PANEL_H = 360;

// Round panel: a circle of r=180 inscribes a 254x254 square. Anything
// outside this box risks being cut off by the bezel.
static const int16_t SAFE_X = 53;
static const int16_t SAFE_Y = 53;
static const int16_t SAFE_W = 254;
static const int16_t SAFE_H = 254;

extern Arduino_ST77916_Waveshare lcdPanel;

// 0 = off (this board has no PMIC rail to cut), 255 = full.
void panelBacklight(uint8_t duty);

// Call once immediately after the canvas's begin().
//
// Currently a no-op hook, kept because it is already threaded through both
// tests and main. Colour inversion was tried here and is NOT the fix for the
// black-level line artifact: both flipping the init table's 0x21 to 0x20 and
// calling invertDisplay() after begin() invert the entire display (black
// backgrounds turn white). Waveshare's 0x21 gives the correct polarity.
void panelAfterBegin();
