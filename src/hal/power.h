// Power / backlight HAL — replaces the M5StickC Plus's AXP192 PMIC.
//
// This board has no PMIC. The AXP calls the firmware made map as follows:
//   M5.Axp.ScreenBreath(n)  -> LEDC duty on the backlight pin
//   M5.Axp.SetLDO2(false)   -> duty 0 (there is no rail to cut)
//   M5.Axp.PowerOff()       -> deep sleep, waking on a touch
//   M5.Axp.GetBatVoltage()  -> not implemented; the battery ADC on GPIO 1 is
//                              deliberately out of scope, so the BLE status
//                              ack omits its `bat` block (REFERENCE.md
//                              permits omitting fields you don't have)
#pragma once

#include <Arduino.h>

// Percent, 0..100. 0 blanks the panel.
void powerSetBrightness(uint8_t pct);

// Latch the panel dark without losing the framebuffer.
void powerScreenOff();
void powerScreenOn();

// Deep sleep; a touch on the screen wakes and restarts the firmware.
void powerOff();

// Internal die temperature, for the info screen's old AXP temp readout.
float powerTempC();
