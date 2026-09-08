// Software clock — replaces the M5StickC Plus's BM8563 RTC.
//
// This board has no RTC, so the wall clock comes entirely from the desktop's
// one-shot {"time":[epoch, tz_offset_seconds]} message on connect and is kept
// by the system clock afterwards. It does not survive a power cycle; the
// clock screen shows nothing until the bridge connects and pushes the time.
#pragma once

#include <Arduino.h>
#include <time.h>

// epoch = UTC seconds, tzOffsetSec = local offset east of UTC (so US
// Pacific in winter is -28800), exactly as the protocol sends them.
void softclockSet(uint32_t epoch, int32_t tzOffsetSec);

// True once the desktop has pushed a time.
bool softclockValid();

// Local broken-down time. Zeroed if !softclockValid().
void softclockNow(struct tm* out);
