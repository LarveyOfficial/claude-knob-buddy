#include "hal/tft_compat.h"
#include <esp_mac.h>
#include "hal/input.h"
#include "hal/haptics.h"
#include "hal/power.h"
#include "hal/softclock.h"
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

// The canvas is the sprite: one full-frame RGB565 buffer in PSRAM, pushed by
// pushSprite(). Constructed globally because Arduino_GFX constructors only
// latch configuration; the framebuffer is allocated by createSprite() in
// setup(), exactly where the original allocated its sprite.
TFT_eSprite spr(PANEL_W, PANEL_H, &lcdPanel);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  // Apply the identity rotation before anything reads a MAC. Rotation 0 is
  // the factory address, so a device that has never needed recovery keeps
  // the address and name it shipped with.
  uint8_t rot = btRotLoad();
  if (rot) {
    uint8_t base[6] = {0};
    if (esp_efuse_mac_get_default(base) == ESP_OK) {
      // Perturb only the NIC-specific bytes and leave the OUI alone. An
      // earlier version set the locally-administered bit in base[0], which
      // stopped the device advertising at all: a BLE public address is
      // meant to be IEEE-assigned, and the controller does not accept one
      // with that bit flipped.
      base[5] ^= rot;
      base[4] ^= (uint8_t)(rot * 31);
      esp_base_mac_addr_set(base);
    }
  }

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  Serial.printf("[ble] identity rotation %u -> %s\n", rot, btName);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = PANEL_W, H = PANEL_H;   // 360x360 round
const int CX = W / 2;
const int CY_BASE = H / 2;
// No software-controllable LED on this board (the only one is a charge
// indicator wired to the charger IC), so the attention state nudges the
// haptic motor instead of blinking. GPIO 10 here is the touch reset line.

// Round-panel layout. Text laid out for the stick's rectangular 135x240
// panel gets clipped by the bezel here, especially near the top and bottom
// where the circle narrows fast. rowHalf() gives the usable half-width at a
// given y so rows can be inset to match the curve instead of guessing.
static const int PANEL_R = 174;          // 180 radius, minus a 6px margin
static int rowHalf(int y) {
  int dy = y - (PANEL_H / 2);
  int r2 = PANEL_R * PANEL_R - dy * dy;
  if (r2 <= 0) return 0;
  return (int)sqrtf((float)r2);
}
static inline int rowLeft(int y)  { return (PANEL_W / 2) - rowHalf(y); }
static inline int rowRight(int y) { return (PANEL_W / 2) + rowHalf(y); }

// Approve/deny touch zones on the approval screen: left half denies, right
// half approves. Kept here so the hit test and the drawn buttons agree.
static const int ZONE_Y = 258, ZONE_H = 62;
static inline bool zoneHit(int16_t ty) { return ty >= ZONE_Y && ty <= ZONE_Y + ZONE_H; }

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };

// REFERENCE.md describes `prompt` as "a permission decision is needed", but
// the desktop also raises it for tools that ask the user an open question.
// Those have no yes/no answer, so echoing back "once" or "deny" is
// meaningless - the reply has to happen on the desktop. Show attention and
// say so, rather than offering two buttons that cannot express an answer.
static const char* const QUESTION_TOOLS[] = { "AskUserQuestion" };

static bool promptIsQuestion(const char* tool) {
  if (!tool || !tool[0]) return false;
  for (auto* q : QUESTION_TOOLS) if (strcmp(tool, q) == 0) return true;
  return false;
}
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down nap is gone with the IMU: this board has no accelerometer, so
// there is no way to tell that it has been placed screen-down. `napping`
// stays wired up (stats still track nap time) but is never entered.
static void applyBrightness() { powerSetBrightness(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    powerScreenOn();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

// Named beep() throughout the firmware; it drives the LRA haptic motor here,
// since this board has no buzzer. The `sound` setting gates it as before.
static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) hapticsBeep(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_CONTROLS = 1;   // was BUTTONS; this board has none
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
// Two entries from the original are gone with their hardware: "led" (no
// software-controllable LED on this board) and "clock rot" (no IMU, and a
// round panel has no meaningful orientation). "sound" is relabelled since
// it now gates the haptic motor rather than a buzzer. The Settings struct
// keeps both dropped fields so the NVS layout is unchanged.
const char* settingsItems[] = { "brightness", "haptics", "bluetooth", "wifi", "transcript", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 8;

bool    resetOpen = false;
uint8_t resetSel  = 0;
// "clear pairing" is not in upstream. It is needed because the desktop's
// Forget button sends {"cmd":"unpair"}, which travels over an encrypted
// characteristic - so once a bond desyncs and auth starts failing, there is
// no way to reach it from the desktop side. Factory reset would clear the
// bond but also formats the filesystem, taking the installed character with
// it. This clears only the stored LTKs.
const char* resetItems[] = { "delete char", "clear pairing", "factory reset", "back" };
const uint8_t RESET_N = 4;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.hud = !s.hud; break;
    case 5: nextPet(); return;
    case 6: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 7: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 3) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else if (idx == 1) {
    // clear pairing: drop our stored LTKs *and* rotate the BLE address.
    // Dropping the bond alone is not enough - the host keeps its own copy on
    // disk and refuses to re-pair a device it thinks it knows, so the link
    // dies before authentication (HCI reason 0x13). A new address makes us a
    // device it has never seen. Settings, stats and the installed character
    // all survive; the advertised name changes, so pick the new one in the
    // desktop picker.
    bleClearBonds();
    btRotBump();
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
// Panel metrics for the 360x360 face. All three list panels (menu,
// settings, reset) share them so they stay visually consistent.
const int MENU_HINT_H = 20;
const int MENU_ITEM_H = 22;    // size-2 text
const int MENU_W      = 240;
// Labels default to the new input mapping: tap the screen to move the
// selection, turn the knob to act on it.
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "turn", const char* rightLbl = "tap") {
  spr.drawFastHLine(mx + 8, hy - 5, mw - 16, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 10;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 6;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
  spr.setTextSize(2);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = 16 + SETTINGS_N * MENU_ITEM_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(2);
  Settings& s = settings();
  // Indices track settingsItems[]: 0 brightness, 1 haptics, 2 bluetooth,
  // 3 wifi, 4 transcript, 5 ascii pet, 6 reset, 7 back. `led` and
  // `clock rot` were removed from the list, so the toggle run is 1..4.
  bool vals[] = { s.sound, s.bt, s.wifi, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    int y = my + 10 + i * MENU_ITEM_H;
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, y);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 60, y);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 4) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 5) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 14, "turn", "tap");
  spr.setTextSize(1);
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = 16 + RESET_N * MENU_ITEM_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, HOT);
  spr.setTextSize(2);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 10 + i * MENU_ITEM_H);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
  spr.setTextSize(1);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: powerOff(); break;   // deep sleep, wakes on touch
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_CONTROLS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = MENU_W, mh = 16 + MENU_N * MENU_ITEM_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 8, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 8, p.textDim);
  spr.setTextSize(2);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 10 + i * MENU_ITEM_H);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? " on" : " off");
  }
  drawMenuHints(p, mx, mw, my + mh - 14);
  spr.setTextSize(1);
}

// Clock state. Two things the M5StickC Plus had are gone here:
//
//   - the BM8563 RTC. Wall time arrives only via the desktop's one-shot
//     {"time":[epoch,tz]} on connect and is kept by the system clock
//     (hal/softclock). It does not survive a power cycle.
//   - the IMU, and with it orientation detection. The original rotated into
//     a landscape clock face when stood on its side; a round 360x360 panel
//     has no meaningful orientation, so there is one fixed face and the
//     `clock rot` setting is gone.
static struct tm _clk = {};
uint32_t         _clkLastRead = 0;   // zeroed by data.h on time-sync
// Without a battery ADC (deliberately out of scope) there is no way to sense
// USB power. The original used it to keep the clock face up while charging
// and to suppress the idle screen-off; treat the board as always mains-fed,
// which is how a desk knob is actually used.
static const bool _onUsb = true;

static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  softclockNow(&_clk);
}

static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clk.tm_wday % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u",
                       (unsigned)_clk.tm_hour, (unsigned)_clk.tm_min);
  uint8_t mi = (_clk.tm_mon >= 0 && _clk.tm_mon <= 11) ? _clk.tm_mon : 0;
  char dl[20]; snprintf(dl, sizeof(dl), "%s %s %02u  :%02u",
                        DOW[clockDow()], MON[mi],
                        (unsigned)_clk.tm_mday, (unsigned)_clk.tm_sec);

  // Same strip as the home indicators (pet occupies y 10..214 above).
  const int TOP = 222;
  spr.fillRect(0, TOP - 6, W, H - TOP + 6, p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(5);
  spr.setTextColor(p.text, p.bg);
  spr.drawString(hm, CX, TOP + 34);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString(dl, CX, TOP + 76);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  // Upstream required three concurrent sessions. In practice you almost
  // always have exactly one, so busy.gif never played and the pet sat in
  // the idle carousel while Claude was clearly working.
  if (s.sessionsRunning >= 1)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// Was a shake, read off the IMU. With no accelerometer on this board the
// equivalent gesture is spinning the knob hard - see inputSpun(), which
// measures the rate between detents rather than counting them.
bool checkShake() {
  return inputSpun();
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  int L = rowLeft(y + 8) + 10, R = rowRight(y + 8) - 10;
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(L, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(R - 42, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 20;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(rowLeft(y + 8) + 10, y); spr.print(section);
  y += 22;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("BLUETOOTH PAIRING", CX, 110);
  spr.setTextSize(2);
  spr.drawString("enter on desktop", CX, 250);
  // Six digits at size 5 is 180px wide - comfortably inside the chord at
  // the vertical centre, where the panel is widest.
  spr.setTextSize(5);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.drawString(b, CX, 180);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

void drawInfo() {
  const Palette& p = characterPalette();
  // Size 1 was already tiny on the stick; on this panel (360px across a
  // ~32mm face) a size-1 glyph is about half a millimetre tall, which is
  // not readable. Size 2 throughout, with each row inset to the chord so
  // nothing runs under the bezel.
  const int TOP = 86;
  const int LH = 17;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(2);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(rowLeft(y + 8) + 10, y); spr.print(b); y += LH;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 4;
    ln("I sleep when idle,");
    ln("wake when you work,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Tap APPROVE or DENY");
    ln("right on a prompt.");
    y += 4;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species: Settings");
    ln("> ascii pet.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "CONTROLS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("turn knob");
    spr.setTextColor(p.textDim, p.bg); ln("   move / scroll");
    spr.setTextColor(p.text, p.bg);    ln("tap screen");
    spr.setTextColor(p.textDim, p.bg); ln("   pick / next screen");
    spr.setTextColor(p.text, p.bg);    ln("hold screen");
    spr.setTextColor(p.textDim, p.bg); ln("   open menu");
    spr.setTextColor(p.text, p.bg);    ln("on a prompt");
    spr.setTextColor(p.textDim, p.bg); ln("   tap DENY/APPROVE");
    ln("   spin = dizzy");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    // No battery ADC on this build (GPIO 1 is wired for it but deliberately
    // out of scope), so there is nothing to report. REFERENCE.md permits
    // omitting fields you don't have; the desktop stats panel just shows no
    // battery row.
    int vBat_mV = 0, iBat_mA = 0, vBus_mV = 0;
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
    ln("  temp     %dC", (int)powerTempC());   // S3 internal sensor

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(3);
    spr.setCursor(rowLeft(y + 12) + 10, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(2);
    y += 30;

    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Claude desktop >");
      ln(" Developer >");
      ln(" Hardware Buddy");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("Waveshare Knob 1.8");
    ln("ESP32-S3 + ST77916");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][48], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// Shown instead of the approval screen when the pending prompt is a
// question rather than a permission decision. The pet stays in its
// attention animation above; this only fills the strip below it, so the
// character is what actually catches your eye.
static void drawQuestion(const Palette& p) {
  const int TOP = 222;
  spr.fillRect(0, TOP - 6, W, H - TOP + 6, p.bg);
  // Deliberately the same two type sizes and positions as the clock face,
  // so the strip reads consistently whatever is in it. The prompt hint used
  // to render here at size 1 and was too small to be worth the space - the
  // desktop shows the full question anyway.
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(5);
  spr.setTextColor(p.body, p.bg);
  spr.drawString("question", CX, TOP + 34);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("answer on desktop", CX, TOP + 76);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
}

static void drawApproval() {
  const Palette& p = characterPalette();

  // Full-screen on this panel rather than the stick's bottom-78px overlay.
  // An approval is the one moment the device demands attention, and a round
  // 360x360 face has room to say so legibly - and room for touch targets
  // big enough to hit without looking.
  spr.fillSprite(p.bg);

  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  char hdr[24]; snprintf(hdr, sizeof(hdr), "approve?  %lus", (unsigned long)waited);
  spr.drawString(hdr, CX, 96);

  // Tool name is the headline: as large as fits the chord at this height.
  int toolLen = strlen(tama.promptTool);
  int size = 4;
  while (size > 1 && toolLen * 6 * size > rowHalf(140) * 2 - 16) size--;
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(size);
  spr.drawString(tama.promptTool, CX, 140);

  // Hint wrapped to the chord width at its own row, two lines max.
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  int hintChars = (rowHalf(186) * 2 - 16) / 12;
  if (hintChars > 40) hintChars = 40;
  int hlen = strlen(tama.promptHint);
  char l1[48], l2[48];
  snprintf(l1, sizeof(l1), "%.*s", hintChars, tama.promptHint);
  spr.drawString(l1, CX, 186);
  if (hlen > hintChars) {
    snprintf(l2, sizeof(l2), "%.*s", hintChars, tama.promptHint + hintChars);
    spr.drawString(l2, CX, 210);
  }

  if (responseSent) {
    spr.setTextSize(2);
    spr.setTextColor(p.textDim, p.bg);
    spr.drawString("sent...", CX, ZONE_Y + ZONE_H / 2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextSize(1);
    return;
  }

  // Two touch targets, matching zoneHit()/ZONE_Y. Left denies, right
  // approves - the same left/right split the tap handler tests.
  int gap = 10;
  int zl = rowLeft(ZONE_Y + ZONE_H);            // narrowest row of the band
  int zr = rowRight(ZONE_Y + ZONE_H);
  int half = (zr - zl - gap) / 2;
  spr.fillRoundRect(zl, ZONE_Y, half, ZONE_H, 8, PANEL);
  spr.drawRoundRect(zl, ZONE_Y, half, ZONE_H, 8, HOT);
  spr.fillRoundRect(zl + half + gap, ZONE_Y, half, ZONE_H, 8, PANEL);
  spr.drawRoundRect(zl + half + gap, ZONE_Y, half, ZONE_H, 8, GREEN);

  spr.setTextSize(2);
  spr.setTextColor(HOT, PANEL);
  spr.drawString("DENY", zl + half / 2, ZONE_Y + ZONE_H / 2);
  spr.setTextColor(GREEN, PANEL);
  spr.drawString("APPROVE", zl + half + gap + half / 2, ZONE_Y + ZONE_H / 2);

  // The knob still denies, as it did when it was BtnB.
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("tap a button, or turn the knob to deny", CX, ZONE_Y + ZONE_H + 18);
  spr.setTextDatum(TL_DATUM);
}

// Roughly doubled from the original, which was drawn for a 135px panel.
static void drawTranscript(const Palette& p, int top, int rows, uint8_t size);

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 4, y, 4, col);
    spr.fillCircle(x + 4, y, 4, col);
    spr.fillTriangle(x - 8, y + 2, x + 8, y + 2, x, y + 11, col);
  } else {
    spr.drawCircle(x - 4, y, 4, col);
    spr.drawCircle(x + 4, y, 4, col);
    spr.drawLine(x - 8, y + 2, x, y + 11, col);
    spr.drawLine(x + 8, y + 2, x, y + 11, col);
  }
}

static void drawPetStats(const Palette& p) {
  // Mood / fed / energy moved to the home screen strip; this page carries
  // the numbers plus the transcript, which finally has room to be rendered
  // at size 3 - the home screen never did, and size 2 on a 32mm face is
  // about 1.4mm tall, which is why it was hard to read.
  const int TOP = 110;
  spr.fillRect(0, TOP - 20, W, H - TOP + 20, p.bg);
  spr.setTextSize(2);

  int y = TOP;
  spr.fillRoundRect(rowLeft(y + 10) + 10, y - 6, 92, 28, 6, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(rowLeft(y + 10) + 20, y + 1);
  spr.printf("Lv %u", stats().level);

  spr.setTextColor(p.textDim, p.bg);
  const int CL = 130;   // counter column, right of the level pill
  spr.setCursor(CL, y - 4);
  spr.printf("ok %u", stats().approvals);
  spr.setCursor(CL, y + 16);
  spr.printf("no %u", stats().denials);

  y += 48;
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(rowLeft(yPx + 8) + 10, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens ", stats().tokens, y);
  tokFmt("today  ", tama.tokensToday, y + 20);

  // Transcript fills the rest of the circle. Size 1 with eight rows shows
  // roughly 300 characters; size 3 with three rows showed about 45, which
  // for real transcript lines is two fragments and no context.
  spr.drawFastHLine(rowLeft(y + 40) + 10, y + 40,
                    rowHalf(y + 40) * 2 - 20, p.textDim);
  drawTranscript(p, y + 52, 8, 1);
  spr.setTextSize(1);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 86;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(2);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* t) {
    spr.setTextColor(c, p.bg);
    spr.setCursor(rowLeft(y + 8) + 10, y);
    spr.print(t); y += 17;
  };
  auto gap = [&]() { y += 5; };

  y += 20;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " nothing kept");
  ln(p.textDim, " waiting = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens = level"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " drains as I work");
  ln(p.textDim, " back up when idle"); gap();

  ln(p.textDim, "idle 30s = screen off");
  ln(p.textDim, "tap = wake");
  spr.setTextSize(1);
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 86;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right,
  // both inset to the chord at this height.
  spr.setTextSize(2);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(rowLeft(y + 10) + 10, y);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(rowRight(y + 10) - 52, y);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
  spr.setTextSize(1);
}

// Transcript rows, rendered wherever a screen wants them. Split out of
// drawHUD() so the pet page can show it at a larger text size than the home
// screen ever had room for.
//
// `size` is the text scale; rows wrap to whatever fits the chord at the
// narrowest row of the block, so nothing runs under the bezel.
static void drawTranscript(const Palette& p, int top, int rows, uint8_t size) {
  // 2px leading, not 8: at size 1 the old formula made rows 16px tall and
  // half the band was blank space.
  const int lh  = size * 8 + 2;
  const int pad = 10;
  const int gw  = size * 6;
  const int area = rows * lh + 6;
  const int width = (rowHalf(top + area) * 2 - pad * 2) / gw;

  spr.fillRect(0, top - 4, W, area, p.bg);
  spr.setTextSize(size);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.drawString(tama.msg, CX, top + lh / 2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextSize(1);
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[32][48];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp,
                           (uint8_t)(width > 47 ? 47 : width));
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > rows) ? (nDisp - rows) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int endRow = (int)nDisp - msgScroll;
  int startRow = endRow - rows; if (startRow < 0) startRow = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; startRow + i < endRow; i++) {
    uint8_t row = startRow + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    int y = top + i * lh;
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(rowLeft(y + size * 4) + pad, y);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    int y = top + (rows - 1) * lh;
    spr.setTextColor(p.body, p.bg);
    spr.setTextSize(1);
    spr.setCursor(rowRight(y + 8) - pad - 18, y + 6);
    spr.printf("-%u", msgScroll);
  }
  spr.setTextSize(1);
}

// Home screen strip under the pet: mood, fed, energy, at a glance.
//
// Deliberately wordless. The equivalent rows on the pet page carry labels,
// but here the shapes carry the meaning (hearts / dots / bars) and dropping
// the labels buys enough width to draw the indicators large enough to read
// across a desk - which text at this physical size is not.
static void drawHomeStats(const Palette& p) {
  const int TOP = 222;
  spr.fillRect(0, TOP - 6, W, H - TOP + 6, p.bg);

  if (tama.promptId[0]) {
    if (promptIsQuestion(tama.promptTool)) { drawQuestion(p); return; }
    drawApproval();
    return;
  }
  if (tama.lineGen != lastLineGen) { lastLineGen = tama.lineGen; wake(); }

  // Four rows now: session count, then mood / fed / energy. The count has
  // to live here - at 360px the area above and beside the pet is all
  // outside the circle, so this strip is the only space available.
  int y = TOP + 4;
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(2);
  char sb[32];
  if (tama.sessionsWaiting > 0)
    snprintf(sb, sizeof(sb), "%u run  %u wait",
             (unsigned)tama.sessionsRunning, (unsigned)tama.sessionsWaiting);
  else if (tama.sessionsRunning > 0)
    snprintf(sb, sizeof(sb), "%u running", (unsigned)tama.sessionsRunning);
  else
    snprintf(sb, sizeof(sb), "%u sessions", (unsigned)tama.sessionsTotal);
  spr.setTextColor(tama.sessionsWaiting  ? HOT
                 : tama.sessionsRunning  ? p.body
                                         : p.textDim, p.bg);
  spr.drawString(sb, CX, y);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);

  y += 30;
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  int x = CX - (4 * 26) / 2 + 13;
  for (int i = 0; i < 4; i++) tinyHeart(x + i * 26, y, i < mood, moodCol);

  y += 26;
  uint8_t fed = statsFedProgress();
  x = CX - (10 * 15) / 2 + 7;
  for (int i = 0; i < 10; i++) {
    if (i < fed) spr.fillCircle(x + i * 15, y, 4, p.body);
    else         spr.drawCircle(x + i * 15, y, 4, p.textDim);
  }

  y += 24;
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  x = CX - (5 * 22) / 2 + 3;
  for (int i = 0; i < 5; i++) {
    if (i < en) spr.fillRect(x + i * 22, y - 6, 16, 12, enCol);
    else        spr.drawRect(x + i * 22, y - 6, 16, 12, p.textDim);
  }
}

void setup() {
  Serial.begin(115200);
  // createSprite() below allocates the framebuffer and begins the QSPI bus;
  // input/haptics share one I2C bus so inputInit() must run first.
  inputInit();
  hapticsInit();
  startBt();
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  if (!spr.createSprite(W, H)) {
    Serial.println("FATAL: framebuffer alloc failed - PSRAM not enabled?");
  }
  panelAfterBegin();
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 28);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 28);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 28);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 28);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  inputUpdate();   // replaces M5.update(): polls touch, drains knob detents

  // A folder push that loses its link must be torn down, or _xActive stays
  // stuck true and the retry after reconnect starts from half-written state.
  {
    static bool wasLinked = false;
    bool linked = bleConnected();
    if (wasLinked && !linked) xferAbort();
    wasLinked = linked;
  }
  t++;
  uint32_t now = millis();

  dataPoll(&tama);

  // Energy integrates against whether Claude is working, which replaces the
  // face-down nap the IMU used to provide.
  statsEnergyUpdate(tama.sessionsRunning > 0);

  // Mood: time how long anything stays blocked, and record it when it
  // clears - whether a human tapped APPROVE or auto mode handled it. With
  // only the manual-approval hook, auto mode never produced a sample.
  {
    static uint32_t waitStartMs = 0;
    if (tama.sessionsWaiting > 0) {
      if (waitStartMs == 0) waitStartMs = millis();
    } else if (waitStartMs != 0) {
      statsOnWaitCleared((millis() - waitStartMs) / 1000);
      waitStartMs = 0;
    }
  }

  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);
  // derive() reaches attention via sessionsWaiting, which covers permission
  // prompts. A question may not set that counter, so key off the prompt
  // itself: anything pending should make the pet look up.
  if (tama.promptId[0] && !responseSent) baseState = P_ATTENTION;

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // The original blinked the red LED while an approval was pending. There is
  // no software-controllable LED here, so nudge the haptic motor instead -
  // slowly, so a prompt left unanswered doesn't buzz continuously.
  if (activeState == P_ATTENTION && settings().sound) {
    static uint32_t lastNudge = 0;
    if (now - lastNudge > 4000) { lastNudge = now; hapticsEffect(7); }
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      Serial.printf("[prompt] tool='%s' question=%d hint='%s'\n",
                    tama.promptTool, promptIsQuestion(tama.promptTool) ? 1 : 0,
                    tama.promptHint);
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHomeStats which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;
  // A question keeps the screen awake and the pet alert, but must not put
  // the approve/deny UI up or let a tap send a decision.
  bool isQuestion = promptIsQuestion(tama.promptTool);
  bool inDecision = inPrompt && !isQuestion;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (BtnA.isPressed() || BtnB.isPressed()) {
    if (screenOff) {
      if (BtnA.isPressed()) swallowBtnA = true;
      if (BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // The stick's third (AXP power) button has no equivalent here. Its only
  // job was toggling the screen off by hand; the 30s idle timeout below
  // already covers that, and "turn off" in the menu handles a real power
  // down via deep sleep.

  if (BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inDecision) {
        // The stick had two physical buttons, so approve/deny was A/B. Here
        // the decision comes from *where* on the screen the tap landed:
        // the DENY and APPROVE targets drawn by drawApproval(). A tap
        // outside that band is ignored rather than guessed at - a
        // mis-tap must never silently approve a tool call.
        int16_t tx, ty;
        if (!touchPoint(&tx, &ty) || !zoneHit(ty)) {
          // Not on a button; fall through without deciding.
        } else {
          bool approve = tx >= CX;
          char cmd[96];
          snprintf(cmd, sizeof(cmd),
                   "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
                   tama.promptId, approve ? "once" : "deny");
          sendCmd(cmd);
          responseSent = true;
          if (approve) {
            // statsOnApproval also records velocity. The wait-cleared timer
            // will fire for the same event a moment later; both measure the
            // same delay, so the duplicate is harmless and keeps the
            // approvals counter honest.
            uint32_t tookS = (millis() - promptArrivedMs) / 1000;
            statsOnApproval(tookS);
            beep(2400, 60);
            if (tookS < 5) triggerOneShot(P_HEART, 2000);
          } else {
            statsOnDenial();
            beep(600, 60);
          }
        }
      } else if (resetOpen) {
        beep(2400, 30);
        applyReset(resetSel);
      } else if (settingsOpen) {
        beep(2400, 30);
        applySetting(settingsSel);
      } else if (menuOpen) {
        beep(2400, 30);
        menuConfirm();
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // Knob detent: moves through things. On the stick this was BtnB and it
  // *activated* the selection while BtnA stepped it; a knob wants the
  // opposite, so the two are swapped here. Because a knob has a direction
  // the lists now scroll both ways instead of only cycling forward.
  if (BtnB.wasPressed()) {
    int dir = inputLastDir() >= 0 ? 1 : -1;
    auto step = [&](uint8_t cur, uint8_t n) -> uint8_t {
      return (uint8_t)((cur + (dir > 0 ? 1 : n - 1)) % n);
    };
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inDecision) {
      // Deny stays on the knob: it is the one decision worth being able to
      // make without aiming at a touch target.
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(1800, 30);
      resetSel = step(resetSel, RESET_N);
      resetConfirmIdx = 0xFF;
    } else if (settingsOpen) {
      beep(1800, 30);
      settingsSel = step(settingsSel, SETTINGS_N);
    } else if (menuOpen) {
      beep(1800, 30);
      menuSel = step(menuSel, MENU_N);
    } else if (displayMode == DISP_INFO) {
      beep(1800, 30);
      infoPage = step(infoPage, INFO_PAGES);
    } else if (displayMode == DISP_PET) {
      beep(1800, 30);
      petPage = step(petPage, PET_PAGES);
      applyDisplayMode();
    } else {
      beep(1800, 30);
      if (dir > 0) msgScroll = (msgScroll >= 30) ? 30 : msgScroll + 1;
      else         msgScroll = (msgScroll == 0)  ? 0  : msgScroll - 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  // Upstream only showed the clock while charging, which on the stick meant
  // "sitting in its cradle". There is no power sensing here, so _onUsb is
  // hardcoded true - and using it directly made the clock take over the
  // moment Claude went quiet and hand back on the next message, which reads
  // as the screen flashing between two layouts. Gate on *sustained* idle
  // instead so the swap is a deliberate, rare event.
  static uint32_t idleSince = 0;
  bool anyActivity = tama.sessionsRunning > 0 || tama.sessionsWaiting > 0;
  if (anyActivity || !tama.connected) idleSince = 0;
  else if (idleSince == 0)            idleSince = now;
  const uint32_t CLOCK_IDLE_MS = 60000;
  bool longIdle = idleSince != 0 && (now - idleSince) > CLOCK_IDLE_MS;

  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && !anyActivity
               && dataRtcValid() && longIdle;
  // The original had a second, landscape clock mode selected by tilting the
  // stick. No IMU and a round panel mean there is only one face now, so the
  // landscapeClock branch is gone entirely.
  static bool wasClocking = false;
  if (clocking != wasClocking) {
    // Upstream put the pet into peek mode here, which halves it - on a
    // 135px panel 48px wide was a third of the screen, but on 360px it is a
    // postage stamp. The clock lives in the same strip the home indicators
    // use, below the pet, so the character keeps its full 2x placement and
    // the transition no longer reloads the GIF at a different scale.
    applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clk.tm_hour;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff) {
    // skip sprite render — powered off (nap is unreachable without an IMU)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.drawString("installing", CX, 150);
      char b[32];
      snprintf(b, sizeof(b), "%luK / %luK", done/1024, total/1024);
      spr.drawString(b, CX, 176);
      // Bar spans the chord at its own row so it stays inside the bezel.
      int barX = rowLeft(200) + 20, barW = rowRight(200) - barX - 20;
      spr.drawRect(barX, 200, barW, 14, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 2) spr.fillRect(barX + 2, 202, fill - 4, 10, p.body);
      }
    } else {
      spr.drawString("no character", CX, 164);
      spr.drawString("loaded", CX, 190);
    }
    spr.setTextDatum(TL_DATUM);
    spr.setTextSize(1);
  }
  if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHomeStats(characterPalette());
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();

    // During a folder push the loop period sets the transfer rate: the
    // desktop waits for an ack per chunk, and an ack only goes out when
    // dataPoll() runs. A full-frame flush is ~26ms of that, so throttle the
    // progress screen to ~4fps and let the remaining iterations be spent
    // draining chunks instead of redrawing a progress bar.
    static uint32_t lastPush = 0;
    if (!xferActive() || millis() - lastPush >= 250) {
      lastPush = millis();
      spr.pushSprite(0, 0);
    }
  }

  // The face-down nap loop lived here. It needed the IMU to detect being
  // placed screen-down, so it is gone; statsOnNapEnd()/statsOnWake() are
  // simply never called and the nap counter stays at whatever NVS holds.

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    powerScreenOff();
    screenOff = true;
  }

  // Loop-period stats. Off by default - build with -DKNOB_LOOP_STATS to
  // re-enable. This is what identified the folder-push bottleneck: 57ms per
  // iteration, and since an ack only goes out when dataPoll() runs, the loop
  // period *was* the transfer rate.
#ifdef KNOB_LOOP_STATS
  {
    static uint32_t prev = 0, worst = 0, lastReport = 0, iters = 0, sum = 0;
    uint32_t t0 = millis();
    if (prev) { uint32_t dt = t0 - prev; sum += dt; iters++; if (dt > worst) worst = dt; }
    prev = t0;
    if (t0 - lastReport > 1000) {
      lastReport = t0;
      Serial.printf("[loop] avg=%lums worst=%lums iters=%lu xfer=%d\n",
                    (unsigned long)(iters ? sum / iters : 0),
                    (unsigned long)worst, (unsigned long)iters, xferActive() ? 1 : 0);
      worst = 0; iters = 0; sum = 0;
    }
  }
#endif

  delay(screenOff ? 100 : (xferActive() ? 1 : 16));
}
