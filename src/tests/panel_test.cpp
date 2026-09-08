// Stage 1 bring-up + background-artifact diagnosis.
//
// Splits the question in two: is the *framebuffer* clean (checkable here over
// serial, no eyes needed), and does a uniform full-frame flush reach the
// panel uniformly (needs eyes). If the buffer verifies all-zero but the panel
// still shows a grid, the fault is panel-side, not in the draw path.
#include "../hal/panel.h"

static Arduino_Canvas canvas(PANEL_W, PANEL_H, &lcdPanel);

static void verifyFill(uint16_t expect, const char* label) {
  uint16_t* fb = canvas.getFramebuffer();
  size_t n = (size_t)PANEL_W * PANEL_H;
  size_t bad = 0;
  uint16_t firstBad = 0; size_t firstBadAt = 0;
  for (size_t i = 0; i < n; i++) {
    if (fb[i] != expect) {
      if (!bad) { firstBad = fb[i]; firstBadAt = i; }
      bad++;
    }
  }
  Serial.printf("[fb] %s: expect 0x%04X, %u/%u pixels wrong",
                label, expect, (unsigned)bad, (unsigned)n);
  if (bad) Serial.printf(", first at %u (x=%u,y=%u) = 0x%04X",
                         (unsigned)firstBadAt,
                         (unsigned)(firstBadAt % PANEL_W),
                         (unsigned)(firstBadAt / PANEL_W), firstBad);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== panel test (artifact diagnosis) ===");
  Serial.printf("psram total=%u free=%u\n",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  if (!canvas.begin()) { Serial.println("canvas.begin() FAILED"); return; }
  canvas.setTextWrap(false);
  panelAfterBegin();
  panelBacklight(255);
  Serial.printf("fb=%p  %dx%d\n", canvas.getFramebuffer(),
                canvas.width(), canvas.height());

  canvas.fillScreen(0x0000);
  verifyFill(0x0000, "after fillScreen(black)");
  canvas.fillScreen(0x001F);
  verifyFill(0x001F, "after fillScreen(blue)");
}

void loop() {
  // Phase 1: pure black, nothing else. Any pattern here is not ours.
  Serial.println("PHASE 1: solid BLACK, no drawing");
  canvas.fillScreen(0x0000);
  canvas.flush();
  delay(5000);

  // Phase 2: pure white. Makes any missing/stale bands obvious in reverse.
  Serial.println("PHASE 2: solid WHITE");
  canvas.fillScreen(0xFFFF);
  canvas.flush();
  delay(5000);

  // Phase 3: black with one centred label, matching how the input test drew.
  Serial.println("PHASE 3: BLACK + centred text");
  canvas.fillScreen(0x0000);
  canvas.setTextSize(3);
  canvas.setTextColor(0x07FF, 0x0000);
  canvas.setCursor(110, 170);
  canvas.print("black?");
  canvas.flush();
  delay(5000);
}
