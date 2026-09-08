#include "input.h"
#include <Wire.h>
#include <esp_timer.h>

VirtualButton BtnA;
VirtualButton BtnB;

void VirtualButton::_set(bool down) {
  if (down && !_down) { _pressedEdge = true; _downAt = millis(); }
  if (!down && _down) { _releasedEdge = true; }
  _down = down;
}

// ---------------------------------------------------------------- touch
static int16_t  _tx = 0, _ty = 0;
static bool     _tValid = false;

static bool touchRead(int16_t* x, int16_t* y) {
  // CST816: read 7 bytes from register 0x00. [2] = finger count,
  // [3..4] = X (12-bit, high nibble in [3]), [5..6] = Y. Register map and
  // the 300kHz bus speed both taken from Waveshare's own demo driver.
  uint8_t d[7];
  Wire.beginTransmission(ADDR_TOUCH);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ADDR_TOUCH, 7) != 7) return false;
  for (uint8_t i = 0; i < 7; i++) d[i] = Wire.read();
  if (!d[2]) return false;
  *x = (int16_t)(((uint16_t)(d[3] & 0x0F) << 8) | d[4]);
  *y = (int16_t)(((uint16_t)(d[5] & 0x0F) << 8) | d[6]);
  return true;
}

bool touchPoint(int16_t* x, int16_t* y) {
  if (!_tValid) return false;
  *x = _tx; *y = _ty;
  return true;
}

// ----------------------------------------------------------------- knob
// The vendor's decoder counts debounced rising edges on each channel
// independently — A means one way, B the other — because this is a
// bidirectional switch knob, not a quadrature encoder. Reimplemented
// compactly here with the same 3ms poll / 2-tick debounce.
// Derived from Espressif's bidi_switch_knob.c (Apache-2.0), as shipped in
// Waveshare's 04_Encoder_Test demo.
//
// The poll runs on an esp_timer, not from the main loop, and that is not
// optional: pushing the 360x360 framebuffer is ~259KB over QSPI, so a loop
// iteration can take tens of milliseconds. Polling from there misses most
// detents outright and makes a fast flick indistinguishable from a slow
// turn. Espressif's original used a timer for exactly this reason.
static const uint32_t KNOB_POLL_US   = 3000;
static const uint8_t  DEBOUNCE_TICKS = 2;
static uint8_t  _prevA = 1, _prevB = 1;
static uint8_t  _dbA = 0, _dbB = 0;
// Detents are queued rather than accumulated: main.cpp reads BtnB as a
// button, so each detent has to surface as its own press/release pulse.
static volatile uint8_t _queued = 0;
static volatile int     _lastDir = 0;

// Fast-spin detection: the dizzy trigger, replacing the IMU shake.
//
// Measures the *rate* directly rather than counting detents in a window. A
// count-in-window threshold turned out to be unworkable to tune: loose
// enough to catch a flick was also loose enough that ordinary scrolling set
// it off. Consecutive detent gaps separate the two cleanly - a deliberate
// turn spaces clicks 80ms+ apart, a hard flick packs them under 30ms - so
// require a run of genuinely fast gaps.
static const uint32_t SPIN_FAST_GAP_MS = 30;
static const uint8_t  SPIN_FAST_RUN    = 5;
static uint32_t _prevDetentMs = 0;
static uint8_t  _fastRun = 0;
static volatile uint32_t _lastGapMs = 0;
static volatile bool _spun = false;

static void knobEdge(int dir) {
  if (_queued < 255) _queued++;
  _lastDir = dir;
  uint32_t now = millis();
  uint32_t gap = now - _prevDetentMs;
  _prevDetentMs = now;
  _lastGapMs = gap;
  if (gap <= SPIN_FAST_GAP_MS) {
    if (++_fastRun >= SPIN_FAST_RUN) { _spun = true; _fastRun = 0; }
  } else {
    _fastRun = 0;
  }
}

static void knobChannel(uint8_t level, uint8_t* prev, uint8_t* db, int dir) {
  if (level == 0) {
    if (level != *prev) *db = 0; else (*db)++;
  } else {
    if (level != *prev && ++(*db) >= DEBOUNCE_TICKS) { *db = 0; knobEdge(dir); }
    else *db = 0;
  }
  *prev = level;
}

static void knobPoll(void*) {
  knobChannel(digitalRead(PIN_ENC_A), &_prevA, &_dbA, +1);
  knobChannel(digitalRead(PIN_ENC_B), &_prevB, &_dbB, -1);
}

static esp_timer_handle_t _knobTimer = nullptr;

static void knobTimerStart() {
  const esp_timer_create_args_t args = {
      .callback = &knobPoll,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "knob",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &_knobTimer) == ESP_OK) {
    esp_timer_start_periodic(_knobTimer, KNOB_POLL_US);
  } else {
    Serial.println("[input] knob timer create failed");
  }
}

bool inputSpun() { bool s = _spun; _spun = false; return s; }

int inputLastDir() { return _lastDir; }

uint32_t inputLastGapMs() { return _lastGapMs; }

// ----------------------------------------------------------------- init
void inputInit() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(10);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(60);

  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, I2C_FREQ_HZ);

  // Put the CST816 into normal (not gesture-only) reporting mode.
  Wire.beginTransmission(ADDR_TOUCH);
  Wire.write((uint8_t)0x00);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  _prevA = digitalRead(PIN_ENC_A);
  _prevB = digitalRead(PIN_ENC_B);
  knobTimerStart();
}

void inputUpdate() {
  BtnA._clearEdges();
  BtnB._clearEdges();

  // Touch -> BtnA. Coordinates stay latched after release so a tap handler
  // running on the release edge can still see where the finger was.
  int16_t x, y;
  bool down = touchRead(&x, &y);
  if (down) { _tx = x; _ty = y; _tValid = true; }
  BtnA._set(down);

  // Each queued detent surfaces as its own one-loop BtnB press, released on
  // the following loop so wasPressed()/wasReleased() both fire exactly once.
  static bool held = false;
  if (held) {
    BtnB._set(false);
    held = false;
  } else if (_queued) {
    _queued--;
    BtnB._set(true);
    held = true;
  }
}
