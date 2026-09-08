#include "softclock.h"
#include <sys/time.h>

static bool _valid = false;

void softclockSet(uint32_t epoch, int32_t tzOffsetSec) {
  struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  // POSIX TZ strings invert the sign of the offset: a zone 8 hours *behind*
  // UTC is written "UTC+8". No DST rule, because the desktop re-pushes the
  // current offset on every connect.
  int32_t inverted = -tzOffsetSec;
  int h = inverted / 3600;
  int m = abs((inverted % 3600) / 60);
  char tz[24];
  snprintf(tz, sizeof(tz), "UTC%+d:%02d", h, m);
  setenv("TZ", tz, 1);
  tzset();

  _valid = true;
  Serial.printf("[clock] set epoch=%lu tz=%s\n", (unsigned long)epoch, tz);
}

bool softclockValid() { return _valid; }

void softclockNow(struct tm* out) {
  if (!_valid) { memset(out, 0, sizeof(*out)); return; }
  time_t now = time(nullptr);
  localtime_r(&now, out);
}
