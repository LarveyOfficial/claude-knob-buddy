#include "haptics.h"
#include <Wire.h>

static bool _present = false;

static void reg8(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADDR_DRV2605);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool hapticsPresent() { return _present; }

void hapticsInit() {
  Wire.beginTransmission(ADDR_DRV2605);
  _present = (Wire.endTransmission() == 0);
  if (!_present) {
    Serial.println("[haptics] DRV2605 not found at 0x5A");
    return;
  }
  reg8(0x01, 0x00);   // MODE: internal trigger, out of standby
  reg8(0x1D, 0xA8);   // FEEDBACK: LRA mode (bit7), default brake/loop gain
  reg8(0x03, 0x06);   // LIBRARY: 6 = LRA effect library
  // Default drive was too weak to feel through the metal case. Raise the
  // rated voltage and clamp so effects run near the actuator's limit.
  reg8(0x16, 0x89);   // RATED_VOLTAGE
  reg8(0x17, 0xFF);   // OD_CLAMP: maximum overdrive headroom
  reg8(0x0C, 0x00);   // GO: idle
  Serial.println("[haptics] DRV2605 ready (LRA, library 6, high drive)");
}

void hapticsEffect(uint8_t effect) {
  if (!_present) return;
  reg8(0x04, effect);   // WAVESEQ1
  reg8(0x05, 0x00);     // WAVESEQ2 = end of sequence
  reg8(0x0C, 0x01);     // GO
}

void hapticsBeep(uint16_t freq, uint16_t dur) {
  (void)freq;   // an LRA resonates at one frequency; duration carries the intent
  // Library 6, biased strong throughout: 47 = strong buzz (most urgent),
  // 14 = strong buzz 1, 1 = strong click for short chirps.
  if (dur >= 200)      hapticsEffect(47);
  else if (dur >= 100) hapticsEffect(14);
  else                 hapticsEffect(1);
}
