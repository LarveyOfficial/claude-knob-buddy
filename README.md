# claude-knob-buddy

A fork of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** — a 360×360 round touch
display with a rotary knob and a metal case.

The upstream firmware targets an M5StickC Plus. This board shares almost
nothing with it, so the port replaces the whole hardware layer while keeping
the protocol, the eighteen ASCII pets, the GIF character support and the
screens intact.

> **Building your own device instead?** You don't need this repo either. See
> upstream **[REFERENCE.md](REFERENCE.md)** for the wire protocol — it is
> board-independent and is kept here unmodified.

## What changed from upstream

| | M5StickC Plus (upstream) | Knob 1.8 (this fork) |
| --- | --- | --- |
| Panel | 135×240 SPI ST7789 | **360×360 round QSPI ST77916** |
| Graphics | TFT_eSPI | **Arduino_GFX** — TFT_eSPI cannot drive QSPI |
| Input | BtnA, BtnB, power button | **rotary knob + CST816 touch** |
| Motion | MPU6886 IMU | none |
| Power | AXP192 PMIC | LEDC backlight PWM |
| Feedback | piezo buzzer, red LED | **DRV2605 LRA haptics** |
| Clock | BM8563 RTC | software clock, set over BLE |

Rather than rewrite the UI, `src/hal/tft_compat.h` provides the ~24 TFT_eSPI
calls the firmware actually makes on top of `Arduino_Canvas`. Nineteen of them
already exist on Arduino_GFX with identical signatures; only sprite lifecycle
and text anchoring needed code. All eighteen `src/buddies/*.cpp` files are
unchanged apart from one `#include`.

Two features are gone because their hardware is:

- **Shake → dizzy** now triggers on spinning the knob hard.
- **Face-down → nap** is removed entirely; energy recovers while idle.
  The `led` and `clock rot` settings went with the LED and the IMU.

## Hardware

Pins below are confirmed against Waveshare's own demo code.

| Peripheral | Pins |
| --- | --- |
| ST77916 QSPI | CLK 13, D0 15, D1 16, D2 17, D3 18, CS 14, RST 21 |
| Backlight | GPIO 47 (LEDC, 50 kHz, 8-bit) |
| CST816 touch | SDA 11, SCL 12, INT 9, RST 10 — I²C `0x15` |
| DRV2605 haptics | same I²C bus — `0x5A` |
| Rotary knob | A = GPIO 8, B = GPIO 7 (rotation only, no push switch) |

Unused by this firmware: the PCM5100A DAC, TF card slot, PDM microphone, the
battery ADC on GPIO 1, and the board's **second MCU** (an ESP32-U4WDH that
handles Bluetooth audio). Because there is no battery reading, the BLE status
ack reports mains power with no battery percentage.

## Flashing

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
pio run -e knob -t upload --upload-port /dev/cu.usbmodemXXXX
```

**Plug orientation matters.** A CH445P analog switch picks which MCU is on USB
based on which way up the Type-C connector is. You want the ESP32-S3, which
enumerates as `303A:1001` on `/dev/cu.usbmodem*`. If you instead see
`1A86:7523` on `/dev/cu.usbserial*`, that is the secondary ESP32 — flip the
cable over. The port number changes on every replug, so pass `--upload-port`
explicitly. A `PermissionError` right after a successful flash is normal; USB
re-enumerates during the reset.

Two bring-up sketches are included for diagnosing a new board:

```bash
pio run -e paneltest -t upload --upload-port ...   # panel, colours, geometry
pio run -e inputtest -t upload --upload-port ...   # I2C scan, touch, knob, haptics
```

### Build notes worth knowing

- **`pioarduino` 51.03.07 is required.** Stock `platform = espressif32` ships
  ESP-IDF 4.4, which lacks the QSPI panel APIs; Arduino core 3.1+ refactored
  the BLE library away from the Bluedroid types `src/ble_bridge.cpp` uses.
  51.03.07 (Arduino 3.0.7 / IDF 5.1) is the combination that satisfies both.
- **Arduino_GFX is pinned to 1.5.0.** Later versions call ESP-IDF 5.3 symbols
  (`ESP_INTR_CPU_AFFINITY_AUTO`, `dma_burst_size`) that don't exist in 5.1.
- **QSPI runs at 20 MHz, not the library default 40 MHz.** At 40 MHz black
  backgrounds showed a fine line pattern while solid white stayed clean — the
  signature of marginal timing, since a flipped bit lights a pixel against
  black but vanishes against white. 20 MHz costs about 26 ms per full-frame
  flush (~38 fps ceiling) and is completely clean.
- **The ST77916 init table is Waveshare's, not Arduino_GFX's**
  (`src/hal/st77916_waveshare.h`). The built-in table renders diagonal noise
  on this panel: register `0xC4` differs, and Arduino_GFX never sends
  `COLMOD`, which Waveshare's driver injects separately. Colour inversion is
  *not* involved — flipping `0x21` to `0x20` inverts the whole display.

## Pairing

Enable developer mode in Claude for macOS or Windows (**Help →
Troubleshooting → Enable Developer Mode**), then **Developer → Open Hardware
Buddy…**, click **Connect**, and pick `Claude-XXXX`. The device shows a
six-digit passkey to type on the desktop; after that the link is encrypted and
reconnects on its own.

## Controls

|  | Turn knob | Tap screen | Hold screen |
| --- | --- | --- | --- |
| **Home** | scroll transcript | next screen | menu |
| **Menu / settings** | move selection | activate | close |
| **Info / pet** | change page | next screen | menu |
| **Approval** | **deny** | **tap DENY or APPROVE** | menu |

Spin the knob hard to make the pet dizzy. The screen sleeps after 30s idle
(kept awake while an approval is pending); any touch wakes it.

Note the knob scrolls and the tap activates — the opposite of upstream, where
BtnA stepped the selection and BtnB confirmed it.

## Pets and characters

Unchanged from upstream: eighteen ASCII species with seven animations each,
cycled via **Settings → ascii pet**, and GIF character packs dropped onto the
Hardware Buddy window's target. Art is drawn at 3× here rather than 2× — the
widest species row is 17 characters, and 17 × 6 × 4 would overflow 360px.

See upstream's README for the character-pack format; `characters/bufo/` is a
working example and `tools/` is carried over as-is.

## Project layout

```
src/
  hal/                  — everything board-specific lives here
    panel.*             — ST77916 QSPI + PSRAM framebuffer + backlight
    st77916_waveshare.h — Waveshare's 185-command init sequence
    tft_compat.h        — the TFT_eSPI surface, over Arduino_GFX
    input.*             — knob + touch, presented as two buttons
    haptics.*           — DRV2605 (replaces the buzzer and the LED)
    power.*             — brightness, screen off, deep sleep
    softclock.*         — wall clock (replaces the RTC)
  tests/                — panel and input bring-up sketches
  main.cpp              — loop, state machine, UI screens
  buddy.cpp, buddies/   — ASCII species (unchanged but for one include)
  ble_bridge.cpp        — Nordic UART service
  character.cpp         — GIF decode + render
  data.h, xfer.h, stats.h
```

## Availability

The BLE API is only available when the Claude desktop apps are in developer
mode. It is intended for makers and isn't an officially supported product
feature.
