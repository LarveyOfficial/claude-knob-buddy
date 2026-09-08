#include "power.h"
#include "panel.h"
#include "input.h"
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <driver/temperature_sensor.h>

static uint8_t _pct = 100;

void powerSetBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  _pct = pct;
  panelBacklight((uint8_t)((pct * 255) / 100));
}

void powerScreenOff() { panelBacklight(0); }
void powerScreenOn()  { powerSetBrightness(_pct); }

void powerOff() {
  panelBacklight(0);
  // Touch INT is active-low, so wake on level 0. It's an RTC-capable pad on
  // the S3, which ext0 requires.
  rtc_gpio_pullup_en((gpio_num_t)PIN_TOUCH_INT);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TOUCH_INT, 0);
  esp_deep_sleep_start();
}

float powerTempC() {
  static temperature_sensor_handle_t h = nullptr;
  if (!h) {
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &h) != ESP_OK) return 0.0f;
    temperature_sensor_enable(h);
  }
  float c = 0.0f;
  temperature_sensor_get_celsius(h, &c);
  return c;
}
