// Input HAL: knob encoder + CST816 touch, presented as the two-button
// interface the original firmware was written against.
//
// The M5StickC Plus had BtnA (front) and BtnB (side). This board has no
// buttons at all — only a rotary encoder and a single-point capacitive
// touch screen. Rather than rewrite main.cpp's input handling, we
// synthesise objects with the same isPressed/wasPressed/wasReleased/
// pressedFor interface:
//
//   BtnA  <- touch screen  (tap = press; pressedFor(600) = the "hold A"
//                           gesture that opens the menu)
//   BtnB  <- one knob detent, in either direction
//
// The IMU-driven gestures have no equivalent on this board:
//   shake -> dizzy      becomes a fast knob spin (see inputSpun())
//   face-down -> nap    dropped entirely
#pragma once

#include <Arduino.h>

// Touch — CST816, I2C 0x15, bus shared with the DRV2605 haptic driver
static const uint8_t PIN_TOUCH_SDA  = 11;
static const uint8_t PIN_TOUCH_SCL  = 12;
static const uint8_t PIN_TOUCH_INT  = 9;
static const uint8_t PIN_TOUCH_RST  = 10;
static const uint8_t ADDR_TOUCH     = 0x15;
static const uint32_t I2C_FREQ_HZ   = 300000;

// Rotary encoder. Not true quadrature: this is a "bidirectional switch"
// knob where one direction pulses only A and the other only B.
static const uint8_t PIN_ENC_A = 8;
static const uint8_t PIN_ENC_B = 7;

// A button-shaped view over a synthesised press signal.
class VirtualButton {
 public:
  bool isPressed()   const { return _down; }
  bool wasPressed()  const { return _pressedEdge; }
  bool wasReleased() const { return _releasedEdge; }
  bool pressedFor(uint32_t ms) const {
    return _down && (millis() - _downAt) >= ms;
  }
  uint32_t heldMs() const { return _down ? millis() - _downAt : 0; }

  // Called by inputUpdate(); not part of the M5 interface.
  void _set(bool down);
  void _clearEdges() { _pressedEdge = _releasedEdge = false; }

 private:
  bool _down = false, _pressedEdge = false, _releasedEdge = false;
  uint32_t _downAt = 0;
};

extern VirtualButton BtnA;   // touch
extern VirtualButton BtnB;   // knob detent

// Brings up the shared I2C bus, the touch controller and the encoder GPIOs.
void inputInit();

// Poll everything and recompute button edges. Call once per loop, in place
// of the original M5.update().
void inputUpdate();

// Last touch point in panel coordinates. Valid while BtnA.isPressed(), and
// latched through the release so a tap handler can still read where it
// landed. Used for the approval screen's approve/deny hit zones.
bool touchPoint(int16_t* x, int16_t* y);

// True once when the knob was spun hard — the replacement trigger for the
// dizzy state that used to come from shaking the stick. Self-clearing.
bool inputSpun();

// Milliseconds between the two most recent knob detents. Diagnostic: used to
// pick the fast-spin threshold from measured behaviour rather than guesswork.
uint32_t inputLastGapMs();

// Direction of the most recent knob detent: +1 or -1 (0 before the first).
// Read alongside BtnB.wasPressed() when a screen wants to scroll rather than
// just advance.
int inputLastDir();
