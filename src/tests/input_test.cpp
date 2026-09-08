// Stages 2+3 bring-up: I2C scan, touch points, knob detents, haptic effects.
// Build with: pio run -e inputtest -t upload
#include "../hal/panel.h"
#include "../hal/input.h"
#include "../hal/haptics.h"
#include <Wire.h>

static Arduino_Canvas canvas(PANEL_W, PANEL_H, &lcdPanel);

static void i2cScan() {
  Serial.print("i2c scan:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println("   (expect 0x15 touch, 0x5A drv2605)");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== input test ===");
  canvas.begin();
  canvas.setTextWrap(false);
  panelAfterBegin();
  panelBacklight(255);
  inputInit();
  i2cScan();
  hapticsInit();
  Serial.println("turn the knob / touch the screen. haptics fire every 3s.");
}

static int  knobTotal = 0;
static int  tapCount = 0;
static char lastEvt[48] = "waiting...";

void loop() {
  inputUpdate();

  if (BtnB.wasPressed()) {
    knobTotal += inputLastDir();
    snprintf(lastEvt, sizeof(lastEvt), "knob %s  total %d",
             inputLastDir() > 0 ? "RIGHT" : "LEFT", knobTotal);
    // Log the inter-detent gap so the spin threshold can be set from real
    // measurements: scroll normally, then flick hard, and compare.
    Serial.printf("%s  gap=%lums\n", lastEvt,
                  (unsigned long)inputLastGapMs());
  }
  if (BtnA.wasPressed()) {
    int16_t x, y; touchPoint(&x, &y);
    tapCount++;
    snprintf(lastEvt, sizeof(lastEvt), "touch %d,%d  #%d", x, y, tapCount);
    Serial.printf("%s\n", lastEvt);
  }
  if (BtnA.wasReleased()) {
    Serial.printf("touch release after %lums\n", (unsigned long)BtnA.heldMs());
  }
  static bool longFired = false;
  if (BtnA.pressedFor(600) && !longFired) {
    longFired = true;
    Serial.println("LONG PRESS (this is the 'hold A' menu gesture)");
    snprintf(lastEvt, sizeof(lastEvt), "LONG PRESS");
  }
  if (!BtnA.isPressed()) longFired = false;

  if (inputSpun()) {
    Serial.println("SPIN detected (this replaces shake->dizzy)");
    snprintf(lastEvt, sizeof(lastEvt), "SPIN!");
  }

  // Cycle haptic effects so the right ones can be picked by feel.
  static uint32_t lastHaptic = 0;
  static uint8_t effects[] = { 1, 7, 14, 24, 47 };
  static uint8_t ei = 0;
  if (millis() - lastHaptic > 3000) {
    lastHaptic = millis();
    Serial.printf("haptic effect %u\n", effects[ei]);
    hapticsEffect(effects[ei]);
    ei = (ei + 1) % (sizeof(effects) / sizeof(effects[0]));
  }

  // Mirror state on screen so the board is testable without the serial log.
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 60) {
    lastDraw = millis();
    canvas.fillScreen(0x0000);
    canvas.drawCircle(180, 180, 179, 0x2104);
    canvas.setTextSize(2);
    canvas.setTextColor(0x07FF, 0x0000);
    canvas.setCursor(70, 120); canvas.printf("knob %+d", knobTotal);
    canvas.setTextColor(0xFFE0, 0x0000);
    canvas.setCursor(70, 150); canvas.printf("taps %d", tapCount);
    canvas.setTextColor(0xFFFF, 0x0000);
    canvas.setTextSize(1);
    canvas.setCursor(70, 190); canvas.print(lastEvt);
    if (BtnA.isPressed()) {
      int16_t x, y;
      if (touchPoint(&x, &y)) canvas.fillCircle(x, y, 10, 0xF800);
    }
    canvas.flush();
  }
}
