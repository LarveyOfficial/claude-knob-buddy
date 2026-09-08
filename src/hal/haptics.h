// Haptics HAL — DRV2605L LRA driver, I2C 0x5A, sharing the touch bus.
//
// Stands in for two things the M5StickC Plus had and this board doesn't:
//   M5.Beep.tone(freq, dur)  -> a haptic effect chosen by duration
//   the red LED blink on the attention state -> a periodic nudge, since
//   the only LED here is a charge indicator wired to the charger IC
#pragma once

#include <Arduino.h>

static const uint8_t ADDR_DRV2605 = 0x5A;

// Safe to call before inputInit(); it shares that Wire bus, so call after.
void hapticsInit();

// Drop-in for the firmware's beep(freq, dur). Frequency is ignored — an LRA
// has one resonant frequency — but duration selects a longer or sharper
// effect so the alert / confirm / deny chirps stay distinguishable.
void hapticsBeep(uint16_t freq, uint16_t dur);

// Fire a specific DRV2605 waveform (1..123) once.
void hapticsEffect(uint8_t effect);

// True if the chip acked on the bus at init.
bool hapticsPresent();
