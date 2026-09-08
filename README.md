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

## Installing a character pack

**Use USB, not the BLE drop target.** The desktop sends 256-byte chunks and
waits for an ack on each one, which tops out near 3 KB/s; the 569 KB `bufo`
pack needs about three minutes at that rate and the transfer times out
partway through. Measured on this board, the link dropped at ~37%.

```bash
KNOB_PORT=/dev/cu.usbmodemXXXX python3 tools/flash_character.py characters/bufo
```

That stages the pack into `data/` and writes the whole LittleFS image in
under 40 seconds. `uploadfs` replaces the entire filesystem, so the pack
becomes the only character installed.

Because USB flashing bypasses the BLE `char_end` handler that would normally
select the new pack, switch to it once by hand: **hold the screen → turn to
`ascii pet` → tap past the 18 species to the GIF entry**.

Character art is upscaled by the largest integer factor that fits the home
box, so the 96px-wide packs authored for the stick render at 2x (192x200)
here rather than as a postage stamp.

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
| **Home** | — | next screen | menu |
| **Pet / info** | change page | next screen | menu |
| **Menu / settings** | move selection | activate | close |
| **Approval** | **deny** | **tap DENY or APPROVE** | menu |

Screens differ from upstream, which put the transcript on the home screen
under the pet. On a round face that band is only ~200px wide, so entries
fragmented across rows and were too small to read. Instead:

- **Home** — the character, with a session line and then mood / fed /
  energy as hearts, dots and bars underneath. The indicators carry no
  labels: the shapes are the legend, and the width saved lets them be big
  enough to read at a glance. The session line changes wording and colour
  with state — `2 run  1 wait` in red-orange when something is blocked on
  you, `2 running` in the body colour while Claude works, `3 sessions`
  dimmed when idle.
- **Pet page** — level, approve/deny counts, token totals, then the
  transcript at size 1 across eight rows, which shows roughly 300
  characters. (`wrapInto` wrote into a 24-byte row buffer sized for the
  stick, so rows truncated at 23 characters regardless of panel width;
  widening it is what makes the extra room usable.)

The clock face, when it appears, occupies that same strip below the pet
rather than taking the lower half of the screen. Upstream shrank the
character into "peek" mode for the clock, which halves it — fine at 135px
wide, a postage stamp at 360px.

Spin the knob hard to make the pet dizzy.

The clock face takes over after 60s with no Claude activity. Upstream gated
it on USB charging; there is no power sensing here, so it is gated on
sustained idle instead — driving it directly off "always on mains" made the
clock and the transcript swap places on every message, which reads as the
screen flashing.

### Questions vs permission prompts

`REFERENCE.md` describes the `prompt` field as "a permission decision is
needed", but the desktop also raises it for tools that ask an open question
(observed: `prompt.tool` is `AskUserQuestion`, with `hint` empty). Those have
no yes/no answer, so echoing `once` or `deny` back would answer something
that was never asked. This fork checks `prompt.tool` and, for a question,
shows the character in its `attention` animation with a "question / answer
on desktop" banner instead of the approve/deny screen, and ignores taps and
knob turns so no decision can be sent.

`busy` triggers on one running session rather than upstream's three, since
one session is the common case and the animation otherwise never played.
Single-GIF states also loop instead of freezing on their last frame.

### When it will not pair

Symptoms, in the order they are worth checking:

1. **`auth FAIL` in the serial log** — the two sides hold different pairing
   keys. The desktop's Forget button cannot fix this: it sends
   `{"cmd":"unpair"}` over an encrypted characteristic, which is precisely
   what has stopped working.
2. **`disconnected reason=0x13` with `bonds=0`** — the device has no key and
   is asking to pair; the host is refusing and hanging up (`0x13` is
   *remote user terminated*). macOS persists BLE bonds to disk, so it does
   this when it thinks it already knows the device.

The recovery that works:

- **Settings → reset → clear pairing** on the device. This drops the stored
  keys and rotates the BLE address (see below). Settings, stats and the
  installed character all survive.
- **Toggle Bluetooth off and back on** on the Mac, from the menu bar.
- Reconnect and enter the passkey.

Do **not** `sudo pkill bluetoothd`. It does not clear an on-disk bond, and
it leaves Bluetooth in a state where nothing is discoverable until you
toggle it off and on anyway.

**Identity rotation.** `clear pairing` also bumps a counter in NVS that
perturbs the last two bytes of the BLE address, so the host sees a device it
has never met. Rotation 0 is the factory address, so a device that has never
needed recovery keeps the name it shipped with; the advertised
`Claude-XXXX` name changes when it rotates, so pick the new one in the
picker. Only the NIC-specific bytes change — setting the
locally-administered bit in the OUI stops the device advertising entirely,
because a BLE public address is meant to be IEEE-assigned.

Whether rotation is strictly necessary is unproven: the one case observed
was resolved with a rotated address *and* a Bluetooth toggle, and the toggle
alone may have been enough.

Note the knob scrolls and the tap activates — the opposite of upstream, where
BtnA stepped the selection and BtnB confirmed it.

## Stats

Four indicators, and two of them work differently from upstream because the
signals upstream used do not exist here.

| | Source |
| --- | --- |
| **Fed** (10 dots) | Output tokens: one dot per 5K, wrapping every 50K |
| **Level** | 50K tokens each; crossing one fires the celebrate animation |
| **Mood** (4 hearts) | Median of the last 8 "how long did anything sit blocked" times, minus a penalty if denials outweigh approvals |
| **Energy** (5 bars) | Drains one bar per ~30 min with sessions running, recovers one per ~20 min idle |

**Mood** upstream was fed only by manual approvals. With auto-approval on
there is never a sample, and the no-data path returns a fixed middle value —
so mood was not slow to move, it was inert. It now times how long
`waiting` stays above zero and records that when it clears, whether a human
or auto mode cleared it. Same meaning ("is anything stuck?"), works either
way.

**Energy** upstream drained on a timer and only refilled when you lifted the
stick out of a face-down nap. That gesture needs an accelerometer, so on
this board the refill hook had no caller at all and energy fell to zero
after ~6h of uptime and stayed there. Tying it to whether Claude is working
keeps the "sleeps when nothing's happening" idea without needing hardware.

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
