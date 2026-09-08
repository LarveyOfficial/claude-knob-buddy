// TFT_eSPI compatibility surface over Arduino_GFX.
//
// The original firmware targets an M5StickC Plus and draws through TFT_eSPI /
// TFT_eSprite. This board's ST77916 is a QSPI panel, which TFT_eSPI cannot
// drive at all, so drawing goes through Arduino_GFX instead.
//
// Rather than rewrite the UI, we subclass Arduino_Canvas and add back the
// handful of methods TFT_eSPI has and Arduino_GFX doesn't. A grep of every
// source file turns up exactly 24 distinct draw calls, 19 of which already
// exist on Arduino_GFX with identical names and signatures and are simply
// inherited. Only these five need code:
//
//   createSprite / pushSprite / fillSprite   sprite lifecycle -> canvas
//   setTextDatum + drawString                TFT_eSPI has text anchoring,
//                                            Adafruit-style GFX does not
//
// The firmware uses raw RGB565 hex literals throughout and no TFT_* colour
// constants, so none need defining.
//
// Text metrics line up by luck and it matters: TFT_eSPI font 1 and the GFX
// built-in font are both 6x8 with a top-left cursor origin, which is exactly
// what buddy.cpp's BUDDY_CHAR_W / BUDDY_CHAR_H assume. Do not setFont().
#pragma once

#include "panel.h"

// TFT_eSPI text datums. Only these two are used (main.cpp:424,428,446,450,
// 966,980); the rest are omitted deliberately so an unported call site fails
// to compile rather than silently mis-anchoring.
#define TL_DATUM 0
#define MC_DATUM 4

class TFT_eSPI : public Arduino_Canvas {
 public:
  TFT_eSPI(int16_t w, int16_t h, Arduino_G* output)
      : Arduino_Canvas(w, h, output) {}

  // --- sprite lifecycle -------------------------------------------------
  // The canvas IS the sprite: one full-frame RGB565 buffer in PSRAM that
  // every screen draws into and that pushSprite() blits in one go. Same
  // model as the original, which never partially pushed.
  void* createSprite(int16_t w, int16_t h) {
    (void)w; (void)h;   // canvas geometry is fixed at construction
    if (!begin()) return nullptr;
    // TFT_eSPI does not wrap text; Adafruit-style GFX does by default, which
    // would fold the widest ASCII pet rows (17 chars) back onto themselves.
    setTextWrap(false);
    return getFramebuffer();
  }
  void fillSprite(uint16_t color) { fillScreen(color); }
  void pushSprite(int32_t x, int32_t y) {
    (void)x; (void)y;   // always a full-frame blit at 0,0
    flush();
  }

  // --- text anchoring ---------------------------------------------------
  void setTextDatum(uint8_t datum) { _datum = datum; }

  void drawString(const char* str, int32_t x, int32_t y) {
    int16_t bx, by; uint16_t bw, bh;
    getTextBounds(str, 0, 0, &bx, &by, &bw, &bh);
    if (_datum == MC_DATUM) {
      // TFT_eSPI's MC_DATUM treats (x,y) as the centre of the text block.
      // getTextBounds' bx/by are the offset from the cursor to the inked
      // bounding box, so subtract them to land the box, not the cursor.
      setCursor(x - bw / 2 - bx, y - bh / 2 - by);
    } else {
      setCursor(x, y);
    }
    print(str);
  }

 private:
  uint8_t _datum = TL_DATUM;
};

// A distinct type rather than a typedef, so the `class TFT_eSPI;` /
// `class TFT_eSprite;` forward declarations in buddy.h and character.h keep
// working and buddyRenderTo(TFT_eSPI*) still accepts the sprite.
class TFT_eSprite : public TFT_eSPI {
 public:
  TFT_eSprite(int16_t w, int16_t h, Arduino_G* output)
      : TFT_eSPI(w, h, output) {}
};
