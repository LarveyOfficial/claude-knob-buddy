#include "panel.h"
#include "st77916_waveshare.h"

// Arduino_ESP32QSPI's ctor names its data pins after plain SPI
// (mosi/miso/quadwp/quadhd); on a QSPI panel they are simply D0..D3.
static Arduino_ESP32QSPI qspiBus(PIN_LCD_CS, PIN_LCD_CLK,
                                 PIN_LCD_D0, PIN_LCD_D1,
                                 PIN_LCD_D2, PIN_LCD_D3);

// Arduino_GFX 1.5.0's built-in ST77916 table renders diagonal noise on this
// panel, so we use Waveshare's own 185-command sequence instead. See
// st77916_waveshare.h for what differs and why.
Arduino_ST77916_Waveshare lcdPanel(&qspiBus, PIN_LCD_RST, 0 /*rotation*/,
                                   true /*ips*/, PANEL_W, PANEL_H);

void panelBacklight(uint8_t duty) {
  static bool attached = false;
  if (!attached) {
    // Arduino 3.x API; ledcSetup/ledcAttachPin no longer exist.
    ledcAttach(PIN_LCD_BL, 50000, 8);
    attached = true;
  }
  ledcWrite(PIN_LCD_BL, duty);
}

void panelAfterBegin() {
  // Intentionally empty - see the header for why inversion is not the fix.
}
