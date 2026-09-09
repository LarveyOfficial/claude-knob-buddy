#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Header-only with file-static state: include from exactly one translation
// unit (main.cpp). Including from a second .cpp produces duplicate symbols.

// Persistent stats backed by NVS. Load once at boot; save sparingly
// (NVS sectors have ~100K write cycles). We save on significant events
// only — approval, denial, nap end — never on a timer.

static const uint32_t TOKENS_PER_LEVEL = 50000;

struct Stats {
  uint32_t napSeconds;       // cumulative face-down time
  uint16_t approvals;
  uint16_t denials;
  uint16_t velocity[8];      // ring buffer: seconds-to-respond per approval
  uint8_t  velIdx;
  uint8_t  velCount;
  uint8_t  level;
  uint32_t tokens;          // cumulative output tokens, drives level
};

static Stats _stats;
static Preferences _prefs;
static bool _dirty = false;

inline void statsLoad() {
  _prefs.begin("buddy", true);
  _stats.napSeconds = _prefs.getUInt("nap", 0);
  _stats.approvals  = _prefs.getUShort("appr", 0);
  _stats.denials    = _prefs.getUShort("deny", 0);
  _stats.velIdx     = _prefs.getUChar("vidx", 0);
  _stats.velCount   = _prefs.getUChar("vcnt", 0);
  _stats.level      = _prefs.getUChar("lvl", 0);
  _stats.tokens     = _prefs.getUInt("tok", 0);
  size_t got = _prefs.getBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  if (got != sizeof(_stats.velocity)) memset(_stats.velocity, 0, sizeof(_stats.velocity));
  _prefs.end();
  // Level is derived from tokens; if NVS has level set but tokens at 0,
  // backfill so the derivation holds.
  if (_stats.tokens == 0 && _stats.level > 0) {
    _stats.tokens = (uint32_t)_stats.level * TOKENS_PER_LEVEL;
  }
}

inline void statsSave() {
  if (!_dirty) return;
  _prefs.begin("buddy", false);
  _prefs.putUInt("nap", _stats.napSeconds);
  _prefs.putUShort("appr", _stats.approvals);
  _prefs.putUShort("deny", _stats.denials);
  _prefs.putUChar("vidx", _stats.velIdx);
  _prefs.putUChar("vcnt", _stats.velCount);
  _prefs.putUChar("lvl", _stats.level);
  _prefs.putUInt("tok", _stats.tokens);
  _prefs.putBytes("vel", _stats.velocity, sizeof(_stats.velocity));
  _prefs.end();
  _dirty = false;
}

inline void statsMoodOnTokens(uint32_t delta);   // defined below

// Records a response time into the ring buffer that drives mood.
inline void statsRecordVelocity(uint32_t seconds) {
  _stats.velocity[_stats.velIdx] = (uint16_t)min(seconds, (uint32_t)65535u);
  _stats.velIdx = (_stats.velIdx + 1) % 8;
  if (_stats.velCount < 8) _stats.velCount++;
  _dirty = true; statsSave();
}

// Level is token-driven now; approvals only feed mood/velocity.
inline void statsOnApproval(uint32_t secondsToRespond) {
  _stats.approvals++;
  statsRecordVelocity(secondsToRespond);
}

// How long a session sat blocked, regardless of who unblocked it.
//
// Mood used to be fed only by manual approvals, so with auto-approval on
// there was never a sample and statsMoodTier() returned its no-data value
// forever - mood was inert, not merely slow. Timing the whole waiting
// period instead keeps the original meaning ("is anything stuck?") and
// works whether a human or auto mode clears it.
inline void statsOnWaitCleared(uint32_t seconds) {
  statsRecordVelocity(seconds);
}

// Tokens feed the pet. 50K per level, 5K per pip on the fed bar.
// Bridge sends cumulative since its start; we add the delta. A drop means
// the bridge restarted — resync without adding, don't lose NVS progress.
static uint32_t _lastBridgeTokens = 0;
static bool _tokensSynced = false;       // first-sight latch — see below
static bool _levelUpPending = false;

inline void statsOnBridgeTokens(uint32_t bridgeTotal) {
  // The bridge sends its cumulative total since IT started. We track deltas.
  // Bridge restart → number drops → resync. But on DEVICE reboot,
  // _lastBridgeTokens is back to 0 while the bridge's total isn't — first
  // packet would re-credit the entire session. Latch on first sight instead.
  if (!_tokensSynced) {
    _lastBridgeTokens = bridgeTotal;
    _tokensSynced = true;
    return;
  }
  if (bridgeTotal < _lastBridgeTokens) {
    _lastBridgeTokens = bridgeTotal;     // bridge restarted
    return;
  }
  uint32_t delta = bridgeTotal - _lastBridgeTokens;
  _lastBridgeTokens = bridgeTotal;
  if (delta == 0) return;

  statsMoodOnTokens(delta);

  uint8_t lvlBefore = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);
  _stats.tokens += delta;
  uint8_t lvlAfter = (uint8_t)(_stats.tokens / TOKENS_PER_LEVEL);

  // Heartbeats are timer-driven telemetry — don't wear NVS on every delta.
  // Tokens accumulate in RAM, persist only on the milestone. Worst case on
  // hard power-off: lose up to 50K tokens of progress.
  if (lvlAfter > lvlBefore) {
    _stats.level = lvlAfter;
    _levelUpPending = true;
    _dirty = true; statsSave();
  }
}

inline bool statsPollLevelUp() {
  bool r = _levelUpPending;
  _levelUpPending = false;
  return r;
}

inline void statsOnDenial() { _stats.denials++; _dirty = true; statsSave(); }

inline void statsMarkDirty() { _dirty = true; }

inline void statsOnNapEnd(uint32_t seconds) {
  _stats.napSeconds += seconds;
  _dirty = true; statsSave();
}

// Median of the velocity ring buffer. 0 if empty.
inline uint16_t statsMedianVelocity() {
  if (_stats.velCount == 0) return 0;
  uint16_t tmp[8];
  memcpy(tmp, _stats.velocity, sizeof(tmp));
  uint8_t n = _stats.velCount;
  // insertion sort, n ≤ 8
  for (uint8_t i = 1; i < n; i++) {
    uint16_t k = tmp[i]; int8_t j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = k;
  }
  return tmp[n/2];
}

// Mood, 0..4 tiers, held as milli-tiers so it moves smoothly.
//
// Two earlier models both failed on this setup:
//
//   1. Upstream keyed mood off how fast you answered permission prompts.
//      With auto-approval there are no manual approvals, so the sample
//      buffer stayed empty and the no-data path returned a fixed 2/4.
//   2. Timing how long `waiting` stayed above zero. That does not fire
//      either: auto mode approves before a prompt is ever surfaced as
//      waiting, so there was still never a sample - and the old
//      approve/deny ratio penalty then dragged the neutral 2 down to 0,
//      which is worse than where it started.
//
// So mood now tracks work done, which is the one signal that definitely
// flows in every mode: tokens. Using Claude raises it, leaving the pet
// alone lowers it. The approve/deny ratio no longer factors in at all -
// with auto mode the counts are arbitrary, and it was what pinned mood to
// zero.
static const uint32_t MOOD_TOKENS_PER_TIER = 15000;
static const uint32_t MOOD_DECAY_MS = 90UL * 60UL * 1000UL;   // a tier per 90m
static int32_t  _moodMilli   = 2000;   // start neutral at 2/4
static uint32_t _moodLastMs  = 0;

inline void statsMoodOnTokens(uint32_t delta) {
  _moodMilli += (int32_t)((int64_t)delta * 1000 / MOOD_TOKENS_PER_TIER);
  if (_moodMilli > 4000) _moodMilli = 4000;
}

// Call once per loop; decays only while no work is arriving.
inline void statsMoodDecay(bool working) {
  uint32_t now = millis();
  if (_moodLastMs == 0) { _moodLastMs = now; return; }
  uint32_t dt = now - _moodLastMs;
  if (dt < 1000) return;
  _moodLastMs = now;
  if (working) return;
  _moodMilli -= (int32_t)((int64_t)dt * 1000 / MOOD_DECAY_MS);
  if (_moodMilli < 0) _moodMilli = 0;
}

// A prompt left sitting is the one thing that should still sour the mood -
// it is the pet waiting on you, which is the original idea.
inline void statsMoodOnStalled(uint32_t seconds) {
  if (seconds < 60) return;
  _moodMilli -= 500;
  if (_moodMilli < 0) _moodMilli = 0;
}

inline uint8_t statsMoodTier() {
  int32_t t = _moodMilli / 1000;
  if (t < 0) t = 0; if (t > 4) t = 4;
  return (uint8_t)t;
}

// Energy, 0..5.
//
// Upstream drained a tier every 2h and only ever refilled when you picked
// the stick up out of a face-down nap. That gesture needs an accelerometer,
// which this board does not have, so statsOnWake() lost its only caller and
// energy drained to zero after ~6h of uptime and stayed there.
//
// It now tracks work instead: drains while sessions are running, recovers
// while nothing is. Same "sleeps when nothing's happening" idea, no
// hardware needed.
static const uint32_t ENERGY_DRAIN_MS   = 30UL * 60UL * 1000UL;  // per tier
static const uint32_t ENERGY_RECOVER_MS = 20UL * 60UL * 1000UL;  // per tier
static int32_t  _energyMilli   = 3000;   // tiers * 1000, starts at 3/5
static uint32_t _energyLastMs  = 0;

inline void statsEnergyUpdate(bool working) {
  uint32_t now = millis();
  if (_energyLastMs == 0) { _energyLastMs = now; return; }
  uint32_t dt = now - _energyLastMs;
  if (dt < 1000) return;              // integrate at most once a second
  _energyLastMs = now;
  int32_t per = working ? -(int32_t)ENERGY_DRAIN_MS : (int32_t)ENERGY_RECOVER_MS;
  _energyMilli += (int32_t)((int64_t)dt * 1000 / per);
  if (_energyMilli < 0)    _energyMilli = 0;
  if (_energyMilli > 5000) _energyMilli = 5000;
}

// Kept so the name still means something: a full night idle tops you up.
inline void statsOnWake() { _energyMilli = 5000; _energyLastMs = millis(); }

inline uint8_t statsEnergyTier() {
  int32_t t = _energyMilli / 1000;
  if (t < 0) t = 0; if (t > 5) t = 5;
  return (uint8_t)t;
}

inline uint8_t statsFedProgress() {
  return (uint8_t)((_stats.tokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10));
}

// --- Settings --------------------------------------------------------------

struct Settings {
  bool sound;
  bool bt;
  bool wifi;     // placeholder — no WiFi stack linked yet, just stores the pref
  bool led;
  bool hud;
  uint8_t clockRot;  // 0=auto 1=portrait 2=landscape
};

static Settings _settings = { true, true, false, true, true, 0 };

inline void settingsLoad() {
  _prefs.begin("buddy", true);
  _settings.sound = _prefs.getBool("s_snd", true);
  _settings.bt    = _prefs.getBool("s_bt",  true);
  _settings.wifi  = _prefs.getBool("s_wifi",false);
  _settings.led   = _prefs.getBool("s_led", true);
  _settings.hud      = _prefs.getBool("s_hud", true);
  _settings.clockRot = _prefs.getUChar("s_crot", 0);
  if (_settings.clockRot > 2) _settings.clockRot = 0;
  _prefs.end();
}

inline void settingsSave() {
  _prefs.begin("buddy", false);
  _prefs.putBool("s_snd", _settings.sound);
  _prefs.putBool("s_bt",  _settings.bt);
  _prefs.putBool("s_wifi",_settings.wifi);
  _prefs.putBool("s_led", _settings.led);
  _prefs.putBool("s_hud", _settings.hud);
  _prefs.putUChar("s_crot", _settings.clockRot);
  _prefs.end();
}

static char _petName[24] = "Buddy";
static char _ownerName[32] = "";

inline void petNameLoad() {
  _prefs.begin("buddy", true);
  _prefs.getString("petname", _petName, sizeof(_petName));
  _prefs.getString("owner", _ownerName, sizeof(_ownerName));
  _prefs.end();
}

// Strip JSON-breaking chars — these names go into a printf'd JSON string
// unescaped (xfer.h status response). A quote persists to NVS and breaks
// the status endpoint until the name is re-set.
static void _safeCopy(char* dst, size_t dstLen, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
    char c = src[i];
    if (c != '"' && c != '\\' && c >= 0x20) dst[j++] = c;
  }
  dst[j] = 0;
}

inline void petNameSet(const char* name) {
  _safeCopy(_petName, sizeof(_petName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("petname", _petName);
  _prefs.end();
}

inline const char* petName() { return _petName; }

inline void ownerSet(const char* name) {
  _safeCopy(_ownerName, sizeof(_ownerName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("owner", _ownerName);
  _prefs.end();
}

inline const char* ownerName() { return _ownerName; }

inline uint8_t speciesIdxLoad() {
  _prefs.begin("buddy", true);
  uint8_t v = _prefs.getUChar("species", 0xFF);
  _prefs.end();
  return v;
}

inline void speciesIdxSave(uint8_t idx) {
  _prefs.begin("buddy", false);
  _prefs.putUChar("species", idx);
  _prefs.end();
}

// BLE identity rotation counter.
//
// macOS persists BLE bonds to disk keyed by device address, and will not
// re-pair a device it believes it already knows: it reconnects assuming the
// old key, the device asks to pair fresh, and the host terminates the link
// (HCI reason 0x13). Clearing the bond on the device alone cannot fix that -
// it is what creates the mismatch. Restarting bluetoothd does not help
// either, because the bond is on disk.
//
// Bumping this counter derives a new BLE address, so the host sees a device
// it has never met and pairs normally. It must persist, or every boot would
// look like a new device and a bond could never stick.
inline uint8_t btRotLoad() {
  _prefs.begin("buddy", true);
  uint8_t v = _prefs.getUChar("btrot", 0);
  _prefs.end();
  return v;
}

inline void btRotBump() {
  _prefs.begin("buddy", false);
  _prefs.putUChar("btrot", (uint8_t)(_prefs.getUChar("btrot", 0) + 1));
  _prefs.end();
}

inline Settings& settings() { return _settings; }

inline const Stats& stats() { return _stats; }
