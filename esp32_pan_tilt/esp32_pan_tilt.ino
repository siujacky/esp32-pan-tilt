// ============================================================
//  ESP32 Pan / Tilt Servo Controller  (plain ESP32, no camera)
//  Two hobby servos (pan = GPIO 25, tilt = GPIO 26) driven from a
//  self-contained web D-pad. WiFi tries the home network first and
//  falls back to its own Access Point if that fails. A 128x128 SH1107
//  OLED (I2C) shows 3 auto-cycling status pages.
//
//  Design spec: docs/superpowers/specs/2026-08-05-esp32-pan-tilt-design.md
//  v2 spec    : docs/superpowers/specs/2026-08-06-esp32-pan-tilt-v2-limits-home-glide-design.md
//               (soft limits, home calibration, home-speed glide, NVS persistence)
//  Libraries : WiFi.h, WebServer.h, ESP32Servo.h, U8g2 (SH1107 OLED), Preferences.h (NVS)
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>   // NVS persistence (namespace "pantilt")
#include <Adafruit_NeoPixel.h>   // WS2812B corner LEDs on GPIO 16
#include "web_page.h"        // provides: const char INDEX_HTML[] PROGMEM
#include "servo_backend.h"   // ServoBackend seam: enum Axis, PwmBackend (default) + Lx16aBackend, LX helpers

// ------------------------------------------------------------
//  Section 3 - Configuration constants
// ------------------------------------------------------------

// WiFi - Station credentials are placeholders; real values are pasted in
// locally, NEVER committed to git.
const char*    HOME_SSID          = "YOUR_WIFI_SSID";
const char*    HOME_PASS          = "YOUR_WIFI_PASSWORD";
const char*    AP_SSID            = "ESP32-PanTilt";
const char*    AP_PASS            = "pantilt123";   // must be >= 8 chars (WPA2)
const uint32_t WIFI_STA_TIMEOUT_MS = 15000;

// Servos
// PWM servo pins: FIRST-BOOT defaults only - the live values are NVS-backed
// ("ppin"/"tpin"), web-settable from the Digital-servo tab, applied at boot.
const int  PWM_PAN_DEFAULT  = 25;
const int  PWM_TILT_DEFAULT = 26;
int  pwmPanPin  = PWM_PAN_DEFAULT;
int  pwmTiltPin = PWM_TILT_DEFAULT;
const int  SERVO_MIN_US = 500;    // full SG90 travel (original used 1000-2000 = half range)
const int  SERVO_MAX_US = 2500;
const int  SERVO_MIN_DEG = 0;
const int  SERVO_MAX_DEG = 180;
const int  HOME_DEG      = 90;    // center
const bool INVERT_PAN    = false; // flip if a servo moves the wrong way
const bool INVERT_TILT   = false;

// Control
int        stepSize      = 5;     // degrees per nudge - the "speed" control
const int  STEP_MIN      = 1;
const int  STEP_MAX      = 30;

// Status LED (-1 disables)
const int  STATUS_LED_PIN = 2;

// LED blink cadence (ms per toggle): fast while connecting, slow while in AP.
const uint32_t LED_FAST_MS = 150;   // ~3 Hz fast-blink = connecting
const uint32_t LED_SLOW_MS = 500;   // ~1 Hz slow-blink = AP mode

// OLED (SH1107 128x128, I2C) on the ESP32 default I2C pins: SDA 21 / SCL 22.
// setupOLED() also auto-tries the swapped order, so a mixed-up SDA/SCL still works.
const int      PIN_OLED_SDA = 21;
const int      PIN_OLED_SCL = 22;
const uint32_t OLED_PAGE_MS = 3000; // auto-cycle interval per page
const uint32_t OLED_DRAW_MS = 150;  // min interval between full-frame redraws

// WS2812B corner LEDs on GPIO 16, wired in this index order:
//   0 = LED1 top-right, 1 = LED2 top-left, 2 = LED3 bottom-left, 3 = LED4 bottom-right.
// They animate ONLY while a servo is moving, so onlookers can see motion + direction:
//   pan  -> a dot chases clockwise / anti-clockwise around the 4 corners
//   tilt -> a bar wipes bottom->top (up) / top->bottom (down)
const int      PIN_LEDS       = 16;
const int      NUM_LEDS       = 4;
const uint32_t LED_HOLD_MS    = 300;   // motion animation lingers this long after last move
const uint32_t LED_FRAME_MS   = 80;    // motion animation step interval
const uint32_t MOOD_FRAME_MS  = 60;    // idle mood-light update interval (slow rainbow)
const uint8_t  MOOD_VAL       = 76;    // idle mood brightness, ~30% of 255

// I2C joystick: nulllab mini module @ 0x5A (raw registers, no library) - shares the OLED
// I2C bus (21/22). Auto-detected at boot; absent => the cockpit input is simply a no-op.
// (A speculative Adafruit-seesaw path was fully REMOVED 2026-08-09: that hardware was
// never part of this build, and its dead code carried its own review finding.)
const uint32_t JOY_MS       = 30;        // read cadence, ms
const int      JOY_DEAD     = 25;        // deadzone around centre (0-255 scale, centre 128)
const int      JOY_FULL     = 127;       // full deflection from centre
const uint16_t JOY_SLOW_MS  = 220;       // rate-jog nudge interval at min deflection
const uint16_t JOY_FAST_MS  = 40;        // ...at full deflection

// ------------------------------------------------------------
//  Section 4 - Servo motion model: state, servos, backend
//  (enum Axis now lives in servo_backend.h, #included above - still declared
//   before the first function so Arduino's auto-prototypes can see the type.)
// ------------------------------------------------------------
Servo panServo;
Servo tiltServo;
int   panPos  = HOME_DEG;
int   tiltPos = HOME_DEG;

// Servo backends + control-target router (all NVS-persisted; see section 8).
// BOTH backends are instantiated. PwmBackend is always begun; Lx16aBackend only when
// bknd==1 (LX enabled). `ctrlTarget` routes the 6 runtime angle writes: 0=PWM, 1=LX, 2=Both.
// So with bknd==1 both servo sets are live and the web tabs (go=T) switch which the D-pad
// drives - "connect both to test". `backend` points at the active one for telemetry/OLED.
int   bknd       = 0;    // LX enabled at boot? 0 = PWM only (default), 1 = LX-16A bus up too
int   ctrlTarget = 0;    // routing: 0 = PWM, 1 = LX-16A, 2 = Both (mirror). NVS "ctgt"
bool  joyMini    = false;// nulllab mini-joystick (raw I2C @ 0x5A) detected at boot
int   joyEnabled = 1;    // NVS "joyen"; 0 = ignore the stick even if present
int   panid  = 1;        // LX-16A pan  servo bus ID
int   tiltid = 2;        // LX-16A tilt servo bus ID
int   lxpin  = LX_PIN_DEFAULT;   // live LX bus GPIO; NVS "lxpin", web-settable (takes effect on reboot)
int   lxRelax = 0;               // 1 = release servo torque ~3s after a move settles (stops the
                                 // hold-controller's stall-push buzz); NVS "lxrelax", web-settable.
                                 // CAUTION: a released axis does not hold against gravity/loads.
PwmBackend    pwmB;                 // always constructed + begun
Lx16aBackend  lxB;                  // constructed always; begun only if bknd==1
ServoBackend* backend = &pwmB;      // active backend (telemetry/name/OLED); set in setup()

// (driveAngle is defined below ctrlSlot: in "Both" mode each rig is clamped to ITS OWN
// slot's soft-limit window, which needs the slot array in scope - review finding.)

// Per-target saved state (D-A2 full): each target is an INDEPENDENT controller - its own
// position AND its own config (limits/home/speed/step), because the backends "may control
// different things". The live globals (panPos/tiltPos/homing + panMin..stepSize) always
// belong to the ACTIVE target; switching target snapshots them into the outgoing slot and
// restores the incoming one (saveSlot/loadSlot, called from setCtrlTarget).
struct CtrlState {
  int      pan, tilt;                            // position
  bool     homing; uint32_t homeLastMs;          // glide state
  int      panMin, panMax, tiltMin, tiltMax;     // config: soft-limit window
  int      homePan, homeTilt, homeSpeed, stepSize;
};
// OLED hint-bar token (btn -1 = plain text). Defined HERE, before any function, and the
// prototype is written MANUALLY so the Arduino auto-prototype generator (which injects
// its block ahead of this struct) skips hintBar instead of hoisting Hint above its type.
struct Hint { int btn; const char* act; };
void hintBar(int y, const Hint* h, int n);

#define CTRL_DEFAULTS { HOME_DEG, HOME_DEG, false, 0, \
                        SERVO_MIN_DEG, SERVO_MAX_DEG, SERVO_MIN_DEG, SERVO_MAX_DEG, \
                        HOME_DEG, HOME_DEG, 90, 5 }
CtrlState ctrlSlot[3] = { CTRL_DEFAULTS, CTRL_DEFAULTS, CTRL_DEFAULTS };

// Router: fan a logical angle write out to the targeted backend(s). LX only if it's up.
// Targets 0/1: the live soft limits already clamped the command upstream (nudge/glide),
// so the write passes through. Target 2 "Both" (review finding): the shared command was
// clamped only by slot 2's window, so a rig calibrated NARROWER in its own slot could be
// driven past its physical stop while mirroring - here each rig is clamped to its OWN
// slot's window (slots 0/1 cannot change while target 2 is active, so they are in sync).
void driveAngle(Axis a, int deg) {
  if (ctrlTarget == 0) { pwmB.writeAngle(a, deg); return; }
  if (ctrlTarget == 1) { if (bknd == 1) lxB.writeAngle(a, deg); return; }
  int dp = (a == PAN) ? constrain(deg, ctrlSlot[0].panMin,  ctrlSlot[0].panMax)
                      : constrain(deg, ctrlSlot[0].tiltMin, ctrlSlot[0].tiltMax);
  pwmB.writeAngle(a, dp);
  if (bknd == 1) {
    int dl = (a == PAN) ? constrain(deg, ctrlSlot[1].panMin,  ctrlSlot[1].panMax)
                        : constrain(deg, ctrlSlot[1].tiltMin, ctrlSlot[1].tiltMax);
    lxB.writeAngle(a, dl);
  }
}

// ------------------------------------------------------------
//  Section 4.5 (v2) - Soft limits, home calibration, glide, NVS
//    All of these are persisted to NVS and restored (validated) on boot.
// ------------------------------------------------------------
int  panMin    = 0,  panMax  = 180;   // pan  soft limits (clamp window)
int  tiltMin   = 0,  tiltMax = 180;   // tilt soft limits (clamp window)
int  homePan   = 90, homeTilt = 90;   // calibrated home target
int  homeSpeed = 90;                  // home glide speed, deg/sec
const int HSPEED_MIN = 10;
const int HSPEED_MAX = 300;

bool        homing     = false;       // a smooth home glide is in progress
uint32_t    homeLastMs = 0;           // time base for the non-blocking glide stepper
Preferences prefs;                    // NVS handle for namespace "pantilt"

// ---- Per-target config plumbing (D-A2) ---------------------------------------------
// NVS key for one target's copy of a config scalar: base + target digit, e.g. "pmin1".
// NOTE: returns a shared static buffer - use at most ONE ckey() per statement.
const char* ckey(const char* base, int t) {
  static char k[16];
  snprintf(k, sizeof(k), "%s%d", base, t);
  return k;
}

// Snapshot the live globals into slot t (called for the OUTGOING target).
void saveSlot(int t) {
  ctrlSlot[t] = { panPos, tiltPos, homing, homeLastMs,
                  panMin, panMax, tiltMin, tiltMax,
                  homePan, homeTilt, homeSpeed, stepSize };
}

// Make slot t's position + config the live globals (called for the INCOMING target).
void loadSlot(int t) {
  panPos    = ctrlSlot[t].pan;       tiltPos    = ctrlSlot[t].tilt;
  homing    = ctrlSlot[t].homing;    homeLastMs = homing ? millis() : ctrlSlot[t].homeLastMs;
  panMin    = ctrlSlot[t].panMin;    panMax     = ctrlSlot[t].panMax;
  tiltMin   = ctrlSlot[t].tiltMin;   tiltMax    = ctrlSlot[t].tiltMax;
  homePan   = ctrlSlot[t].homePan;   homeTilt   = ctrlSlot[t].homeTilt;
  homeSpeed = ctrlSlot[t].homeSpeed; stepSize   = ctrlSlot[t].stepSize;
}

// ------------------------------------------------------------
//  Section 5/6 - WiFi + HTTP server state
// ------------------------------------------------------------
WebServer server(80);
bool      apMode = false;   // false = STA, true = AP fallback
IPAddress ip;               // active IP (STA local or AP 192.168.4.1)

// ------------------------------------------------------------
//  Section 7 - OLED display state (SH1107 128x128, hardware I2C)
//  Driver variant sets the panel's column offset. The generic ..._128X128 uses
//  x_offset 96 (equivalent to SH1107 reg 0xD3 = 0x60) which shifted THIS panel
//  ~32px right; the PIMORONI variant uses offset 0 (0xD3 = 0x00) and centres it
//  (confirmed on-panel). Alternatives if a different panel shifts/mirrors:
//  U8G2_SH1107_SEEED_128X128_F_HW_I2C or plain U8G2_SH1107_128X128_F_HW_I2C.
// ------------------------------------------------------------
U8G2_SH1107_PIMORONI_128X128_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
bool     oledPresent  = false;  // true once the panel ACKs on the bus
uint8_t  oledAddr     = 0x3C;   // auto-detected (0x3C or 0x3D)
uint8_t  oledPage     = 0;      // current page 0..2 (auto-cycles)
bool     oledDirty    = true;   // redraw requested
uint32_t oledLastPage = 0;      // millis of last page switch
uint32_t oledLastDraw = 0;      // millis of last redraw

// ------------------------------------------------------------
//  Section 7b - Cockpit UI (joystick-driven). C toggles the two modes:
//    DRIVE - stick flies pan/tilt (RATE jog or ABSOLUTE glide), A homes.
//    MENU  - stick U/D = item, L/R = page, OK = edit, B = back. (P4)
//  DRIVE returns to MENU after 5s of stick inactivity (D-G).
// ------------------------------------------------------------
enum CockMode { CM_DRIVE = 0, CM_MENU = 1 };
int      cockMode = CM_MENU;    // boot into MENU (safe: not flying on power-up)
int      menuPage = 0;          // 0 = Config; 1..n = info pages (Position/Calib/Conn/Bus)
int      menuSel  = 0;          // selected item on an editable page
bool     editing  = false;      // OK pressed -> stick U/D now adjusts the selected item
int      joyMode  = 0;          // 0 = RATE (proportional jog), 1 = ABSOLUTE (glide-to-pos) (D-E)
uint32_t driveIdleSince = 0;    // DRIVE idle-timeout anchor (-> MENU after 5s)

// Button remap (D-B: remappable from the WEB PAGE ONLY - never from the stick itself, so a
// bad mapping can't lock you out of the menu that fixes it). Each action stores a button
// INDEX into the tables below; defaults avoid the stiff centre-click (OK) entirely.
const uint8_t JOY_BTN_REG[5]  = { 0x22, 0x23, 0x21, 0x24, 0x20 };   // A, B, C, D, OK
const char*   JOY_BTN_NAME[5] = { "A",  "B",  "C",  "D",  "OK" };
int mapHome  = 0;   // A  - home (DRIVE)
int mapBack  = 1;   // B  - back / cancel edit (MENU)
int mapEnter = 2;   // C  - confirm / edit (MENU)
int mapTgl   = 3;   // D  - toggle DRIVE <-> MENU (anytime)

// Per-servo MENU page (D-F, extended to the FULL LX-16A setting set - parity with the
// SpaceMaster85/lx-16a and maximkulkin/lewansoul-lx16a tools). The bus read is slow, so
// the config is CACHED and only refreshed while that page is on screen. 14 items in a
// scrollable 5-row window; item 5 (Trim save) is an ACTION (Confirm executes, no edit).
const int SV_ITEMS = 14;      // Servo, BusID, LimMin, LimMax, Trim, TrimSave, Vmin, Vmax,
                              // Tmax, Torque, LED, Alarm, Mode, Speed
int      svSel      = 0;      // which linked servo this page edits: 0 = pan, 1 = tilt
LxConfig svCfg;               // last read (fields are -1 where the read failed)
bool     svCfgValid = false;  // false until one read has completed
uint32_t svCfgAt    = 0;      // millis of that read
static inline bool svIsAction(int item) { return item == 5; }   // Trim save

// Servo Bus page rescan results (Confirm on that page re-runs the scan). Declared
// here, above drawInfoPage's first use - variables don't get auto-prototyped.
LxScanResult oledScan[8];
int          oledScanN = -1;  // -1 = never scanned this boot

// ------------------------------------------------------------
//  Section 8 - WS2812B corner LEDs (motion indicator)
// ------------------------------------------------------------
Adafruit_NeoPixel leds(NUM_LEDS, PIN_LEDS, NEO_GRB + NEO_KHZ800);
const int LED_TR = 0, LED_TL = 1, LED_BL = 2, LED_BR = 3;   // strip index per corner

enum LedAnim { LED_IDLE, LED_PAN_CW, LED_PAN_CCW, LED_TILT_UP, LED_TILT_DOWN, LED_FAULT };
LedAnim  ledAnim        = LED_IDLE;
uint32_t ledActiveUntil = 0;    // motion animation runs while millis() < this
uint32_t ledLastFrame   = 0;
uint8_t  ledStep        = 0;
uint16_t moodHue        = 0;    // idle mood-light hue (slowly drifts)
uint32_t moodLastFrame  = 0;

// ------------------------------------------------------------
//  Section 9 - LX-16A telemetry cache + firmware-derived fault
//  Populated by updateTelemetry() (only when bknd==1 and the rig is idle);
//  surfaced in fullStatus() fields 17-23, the OLED "Servo Bus" page, and the
//  WS2812B LED_FAULT override. -1 = no valid sample / read failed / PWM mode.
// ------------------------------------------------------------
int      telPanVin    = -1, telPanTemp    = -1;   // pan  input voltage mV / temperature C
int      telTiltVin   = -1, telTiltTemp   = -1;   // tilt input voltage mV / temperature C
int      telPanActual = -1, telTiltActual = -1;   // measured position, logical deg
int      servoFault   = -1;                       // firmware-derived fault bitmask (-1 = n/a, 0 = OK)
int      oledPageCount = 3;                        // 3 (PWM) or 4 (LX-16A adds the Servo Bus page)
uint32_t telemLastMs   = 0;                        // telemetry throttle timebase
uint8_t  telemStep     = 0;                        // round-robin selector for the vin/temp reads
int      panFailN      = 0, tiltFailN = 0;         // consecutive read-failure counters (bus-dead latch)
bool     panSeen       = false, tiltSeen = false;  // axis answered at least once this boot: "dead" is
                                                   // only meaningful for a servo that was ever alive -
                                                   // a single-servo rig must not latch a permanent fault
                                                   // (and red LEDs) for the axis that was never fitted
const uint32_t TELEM_MS     = 1500;               // telemetry cadence, ms (idle-only)
const int      STALL_DEG    = 15;                 // |commanded - actual| beyond this => stall fault bit
const int      FAULT_FAIL_N = 4;                  // consecutive read failures before latching bus-dead

// ============================================================
//  Helpers
// ============================================================

// Drive the status LED (no-op if disabled with STATUS_LED_PIN = -1).
void setLed(bool on) {
  if (STATUS_LED_PIN >= 0) digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

// Original project's comma splitter: returns the index-th field of `data`.
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = { 0, -1 };
  int maxIndex = data.length() - 1;
  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// Unified state CSV, returned by BOTH /action and /status so the web page has a
// single parser (applyState). Fields 1-13 are the original contract and NEVER move;
// fields 14-23 are APPENDED for the servo backend (section 7.1) and are backward-
// compatible (applyState guards p.length < 13). Fixed field order:
//   1-13:  mode,ip,pan,tilt,step,panMin,panMax,tiltMin,tiltMax,homePan,homeTilt,homeSpeed,homing
//   14-16: bknd,panid,tiltid
//   17-23: panVin,panTemp,tiltVin,tiltTemp,panActual,tiltActual,fault  (all -1 in PWM mode)
//   e.g.  STA,192.168.1.42,90,90,5,10,170,0,180,90,90,90,0,0,1,2,-1,-1,-1,-1,-1,-1,-1
String fullStatus() {
  String s;
  s.reserve(112);
  s += (apMode ? "AP" : "STA");
  s += ','; s += ip.toString();
  s += ','; s += panPos;
  s += ','; s += tiltPos;
  s += ','; s += stepSize;
  s += ','; s += panMin;
  s += ','; s += panMax;
  s += ','; s += tiltMin;
  s += ','; s += tiltMax;
  s += ','; s += homePan;
  s += ','; s += homeTilt;
  s += ','; s += homeSpeed;
  s += ','; s += (homing ? 1 : 0);
  // ---- appended servo-backend fields 14-23 (section 7.1) ----
  s += ','; s += bknd;            // 14
  s += ','; s += panid;           // 15
  s += ','; s += tiltid;          // 16
  s += ','; s += telPanVin;       // 17
  s += ','; s += telPanTemp;      // 18
  s += ','; s += telTiltVin;      // 19
  s += ','; s += telTiltTemp;     // 20
  s += ','; s += telPanActual;    // 21
  s += ','; s += telTiltActual;   // 22
  s += ','; s += servoFault;      // 23
  s += ','; s += ctrlTarget;      // 24 (0=PWM 1=LX 2=Both) control-target router
  s += ','; s += (joyMini ? 1 : 0);      // 25 joystick detected
  s += ','; s += joyEnabled;             // 26 joystick enabled
  s += ','; s += joyMode;                // 27 joystick mode (0 RATE / 1 ABSOLUTE)
  s += ','; s += mapHome;                // 28-31 button remap (index 0..4 = A/B/C/D/OK)
  s += ','; s += mapBack;
  s += ','; s += mapEnter;
  s += ','; s += mapTgl;
  s += ','; s += lxpin;                  // 32 LX-16A bus GPIO (applied at boot)
  s += ','; s += lxRelax;                // 33 idle torque release (0 hold / 1 release)
  s += ','; s += pwmPanPin;              // 34 PWM pan GPIO (applied at boot)
  s += ','; s += pwmTiltPin;             // 35 PWM tilt GPIO (applied at boot)
  return s;
}

// ============================================================
//  Persistence (NVS via Preferences, namespace "pantilt")
//  Keys (<=15 chars): pmin pmax tmin tmax hpan htilt hspd step
// ============================================================
// Read every persisted value (falling back to the current default), then
// VALIDATE so a corrupt/hostile NVS blob can never produce an illegal state:
// clamp each to 0..180, swap if min>max, pull home inside limits, clamp speed/step.
void loadSettings() {
  prefs.begin("pantilt", false);          // read/write, created if missing
  panMin    = prefs.getInt("pmin",  panMin);
  panMax    = prefs.getInt("pmax",  panMax);
  tiltMin   = prefs.getInt("tmin",  tiltMin);
  tiltMax   = prefs.getInt("tmax",  tiltMax);
  homePan   = prefs.getInt("hpan",  homePan);
  homeTilt  = prefs.getInt("htilt", homeTilt);
  homeSpeed = prefs.getInt("hspd",  homeSpeed);
  stepSize  = prefs.getInt("step",  stepSize);
  bknd       = prefs.getInt("bknd",   bknd);
  ctrlTarget = prefs.getInt("ctgt",   ctrlTarget);
  joyEnabled = prefs.getInt("joyen",  joyEnabled);
  joyMode    = prefs.getInt("joymd",  joyMode);
  mapHome    = constrain(prefs.getInt("kbhm", mapHome),  0, 4);   // button remap (D-B)
  mapBack    = constrain(prefs.getInt("kbbk", mapBack),  0, 4);
  mapEnter   = constrain(prefs.getInt("kben", mapEnter), 0, 4);
  mapTgl     = constrain(prefs.getInt("kbtg", mapTgl),   0, 4);
  panid      = prefs.getInt("panid",  panid);
  tiltid     = prefs.getInt("tiltid", tiltid);

  panMin  = constrain(panMin,  SERVO_MIN_DEG, SERVO_MAX_DEG);
  panMax  = constrain(panMax,  SERVO_MIN_DEG, SERVO_MAX_DEG);
  tiltMin = constrain(tiltMin, SERVO_MIN_DEG, SERVO_MAX_DEG);
  tiltMax = constrain(tiltMax, SERVO_MIN_DEG, SERVO_MAX_DEG);
  if (panMin  > panMax)  { int t = panMin;  panMin  = panMax;  panMax  = t; }
  if (tiltMin > tiltMax) { int t = tiltMin; tiltMin = tiltMax; tiltMax = t; }
  homePan   = constrain(homePan,  panMin,  panMax);
  homeTilt  = constrain(homeTilt, tiltMin, tiltMax);
  homeSpeed = constrain(homeSpeed, HSPEED_MIN, HSPEED_MAX);
  stepSize  = constrain(stepSize,  STEP_MIN,   STEP_MAX);
  bknd       = constrain(bknd,   0, 1);      // 0 = PWM only, 1 = LX-16A bus up too
  ctrlTarget = constrain(ctrlTarget, 0, 2);  // 0=PWM 1=LX 2=Both
  joyEnabled = constrain(joyEnabled, 0, 1);
  if (bknd != 1) ctrlTarget = 0;             // no LX bus -> PWM only
  panid  = constrain(panid,  0, 253);
  tiltid = constrain(tiltid, 0, 253);    // panid==tiltid is a UI collision WARNING, not auto-fixed (section 8)
  // Load ALL pin settings BEFORE validating any (review finding: validating lxpin
  // first ran its collision check against the compile-time PWM defaults, so a stored
  // lxpin equal to a default-but-relocated PWM pin was silently reset every boot).
  lxpin      = prefs.getInt("lxpin", lxpin);       // all three web-settable, applied at boot
  pwmPanPin  = prefs.getInt("ppin", pwmPanPin);
  pwmTiltPin = prefs.getInt("tpin", pwmTiltPin);
  if (lxPinReject(lxpin)) lxpin = LX_PIN_DEFAULT;  // now checked against the REAL stored PWM pins
  if (pwmPinReject(pwmPanPin,  pwmTiltPin)) pwmPanPin  = PWM_PAN_DEFAULT;   // corrupt/colliding NVS
  if (pwmPinReject(pwmTiltPin, pwmPanPin))  pwmTiltPin = PWM_TILT_DEFAULT;  // can't strand the rig
  lxRelax = constrain(prefs.getInt("lxrelax", lxRelax), 0, 1);            // idle torque release
  lxTrim10Up[0] = constrain(prefs.getInt("cbu0", 0), 0, LX_TRIM_CAP10);   // learned stiction trims
  lxTrim10Dn[0] = constrain(prefs.getInt("cbd0", 0), 0, LX_TRIM_CAP10);
  lxTrim10Up[1] = constrain(prefs.getInt("cbu1", 0), 0, LX_TRIM_CAP10);
  lxTrim10Dn[1] = constrain(prefs.getInt("cbd1", 0), 0, LX_TRIM_CAP10);

  // ---- Per-target config (D-A2). Each target keeps its own copy; the LEGACY shared keys
  // loaded above are the per-target default, so an existing calibration migrates cleanly
  // to all three targets on the first boot of this firmware (nothing appears to reset).
  // Same validation as the shared set, applied per target.
  for (int t = 0; t < 3; t++) {
    CtrlState &c = ctrlSlot[t];
    c.panMin    = prefs.getInt(ckey("pmin",  t), panMin);
    c.panMax    = prefs.getInt(ckey("pmax",  t), panMax);
    c.tiltMin   = prefs.getInt(ckey("tmin",  t), tiltMin);
    c.tiltMax   = prefs.getInt(ckey("tmax",  t), tiltMax);
    c.homePan   = prefs.getInt(ckey("hpan",  t), homePan);
    c.homeTilt  = prefs.getInt(ckey("htilt", t), homeTilt);
    c.homeSpeed = prefs.getInt(ckey("hspd",  t), homeSpeed);
    c.stepSize  = prefs.getInt(ckey("step",  t), stepSize);

    c.panMin  = constrain(c.panMin,  SERVO_MIN_DEG, SERVO_MAX_DEG);
    c.panMax  = constrain(c.panMax,  SERVO_MIN_DEG, SERVO_MAX_DEG);
    c.tiltMin = constrain(c.tiltMin, SERVO_MIN_DEG, SERVO_MAX_DEG);
    c.tiltMax = constrain(c.tiltMax, SERVO_MIN_DEG, SERVO_MAX_DEG);
    if (c.panMin  > c.panMax)  { int x = c.panMin;  c.panMin  = c.panMax;  c.panMax  = x; }
    if (c.tiltMin > c.tiltMax) { int x = c.tiltMin; c.tiltMin = c.tiltMax; c.tiltMax = x; }
    c.homePan   = constrain(c.homePan,   c.panMin,  c.panMax);
    c.homeTilt  = constrain(c.homeTilt,  c.tiltMin, c.tiltMax);
    c.homeSpeed = constrain(c.homeSpeed, HSPEED_MIN, HSPEED_MAX);
    c.stepSize  = constrain(c.stepSize,  STEP_MIN,   STEP_MAX);
    c.pan       = constrain(c.pan,  c.panMin,  c.panMax);   // boot position inside its own window
    c.tilt      = constrain(c.tilt, c.tiltMin, c.tiltMax);
  }
  loadSlot(ctrlTarget);        // the active target's position + config become the live globals
}

// ============================================================
//  Section 4.5 (v2) - Non-blocking home glide + limit consistency
// ============================================================

// Move `cur` toward `tgt` by at most `s` degrees (clamps AT the target).
int stepToward(int cur, int tgt, int s) {
  if (cur < tgt) return min(cur + s, tgt);
  if (cur > tgt) return max(cur - s, tgt);
  return cur;
}

// ------------------------------------------------------------
//  Servo range-test engine (user request): sweep ONE servo slowly across its full
//  physical 0-180 travel, deliberately IGNORING soft limits (that is what a range
//  test is for), at 10 deg/s so nothing gets hurt. While a test runs (or sits
//  halted) ALL normal motion sources are locked out - joystick, D-pad, homing,
//  auto-trim - so nothing fights the sweep ("halt the system").
//  Collision detection (LX only - PWM has no feedback): commanded keeps advancing
//  but the measured angle stops following -> BACK OFF to measured-5deg in the
//  opposite direction of travel immediately, and HALT until /servotest?stop=1.
// ------------------------------------------------------------
int      testKind   = -1;    // -1 idle · 0 PWM pan · 1 PWM tilt · 2 LX servo
int      testLxId   = 1;     // bus ID under test (kind 2)
int      testDeg    = 90;    // current commanded angle
int      testDir    = -1;    // sweep direction
int      testPhase  = 0;     // 0: ->0   1: ->180   2: ->park   (then done)
int      testPark   = 90;    // where to leave the servo when the test completes
bool     testHalted = false; // collision (or manual stop) latched: system stays locked
char     testMsg[72] = "";
uint32_t testStepMs = 0, testChkMs = 0;
int      testActual = -1, testPrevActual = -1, testStallN = 0;

const uint32_t TEST_STEP_MS = 100;  // 1 deg per 100 ms = 10 deg/s - slow by design
const uint32_t TEST_CHK_MS  = 500;  // feedback check cadence (LX)
const int      TEST_ERR_DEG = 8;    // commanded-vs-measured gap that flags a block

bool testBusy() { return testKind >= 0; }   // running OR halted: normal motion stays locked

// B,H: begin a smooth glide to (homePan,homeTilt). The actual stepping happens
// in updateHoming() from loop(), so this never blocks.
void startHome() {
  if (testBusy()) return;                   // range test owns the rig
  homing     = true;
  homeLastMs = millis();
}

// Called every loop() pass. Time-based: advance by homeSpeed*elapsed/1000 deg,
// letting sub-degree time accumulate (never advance the time base until we move
// at least 1 deg). Sets oledDirty so the OLED animates the sweep.
void updateHoming() {
  if (testBusy()) return;                   // range test owns the rig
  if (!homing) return;
  uint32_t now     = millis();
  if (now - homeLastMs > 500) { homeLastMs = now; return; }   // long stall (blocking handler /
                          // WiFi scan): re-base instead of converting the gap into one violent snap
  int      stepDeg = (int)((long)homeSpeed * (now - homeLastMs) / 1000);
  if (stepDeg < 1) return;                 // accumulate: don't move the base yet
  homeLastMs = now;
  int pBefore = panPos, tBefore = tiltPos;
  panPos  = stepToward(panPos,  homePan,  stepDeg);
  tiltPos = stepToward(tiltPos, homeTilt, stepDeg);
  driveAngle(PAN,  panPos);
  driveAngle(TILT, tiltPos);
  oledDirty = true;
  // Corner LEDs: reflect whichever axis is actually moving (pan takes priority).
  if (panPos != pBefore)       signalMove(PAN,  panPos - pBefore);
  else if (tiltPos != tBefore) signalMove(TILT, tiltPos - tBefore);
  if (panPos == homePan && tiltPos == homeTilt) homing = false;
}

// Keep everything mutually consistent after a limit change: home stays inside
// the limits (and is persisted if the clamp actually moved it, so NVS never
// lags the live home), and the live position (and its servo) is pulled inside too.
void reclampToLimits() {
  int hp = constrain(homePan,  panMin,  panMax);
  if (hp != homePan)  { homePan  = hp; prefs.putInt(ckey("hpan",  ctrlTarget), homePan); }
  int ht = constrain(homeTilt, tiltMin, tiltMax);
  if (ht != homeTilt) { homeTilt = ht; prefs.putInt(ckey("htilt", ctrlTarget), homeTilt); }
  int np = constrain(panPos,  panMin,  panMax);
  if (np != panPos)  { panPos  = np; driveAngle(PAN,  panPos); }
  int nt = constrain(tiltPos, tiltMin, tiltMax);
  if (nt != tiltPos) { tiltPos = nt; driveAngle(TILT, tiltPos); }
}

// ---- Calibration setters. Each clamps (min<=max, home within limits),
// ---- persists ONLY its changed key, and (for limits) re-clamps live state.
// (Config setters write the ACTIVE target's own key - see ckey/D-A2.)
void setStep(int v)      { stepSize  = constrain(v, STEP_MIN,   STEP_MAX);   prefs.putInt(ckey("step", ctrlTarget), stepSize); }
void setHomeSpeed(int v) { homeSpeed = constrain(v, HSPEED_MIN, HSPEED_MAX); prefs.putInt(ckey("hspd", ctrlTarget), homeSpeed); }

// Servo-backend identity setters (section 8). Each persists ONLY its own key and is
// kept OUT of resetCalibration (C,RS) - these are hardware identity, not calibration.
void setBknd(int v)   { bknd   = constrain(v, 0, 1);   prefs.putInt("bknd",   bknd); }
void setCtrlTarget(int v) {                            // live routing (no reboot); LX only if it's up
  int nv = constrain(v, 0, 2);
  if (bknd != 1) nv = 0;
  if (nv != ctrlTarget) {                              // swap: save active pos+config, restore the new target's
    saveSlot(ctrlTarget);
    ctrlTarget = nv;
    loadSlot(nv);
    driveAngle(PAN, panPos);  driveAngle(TILT, tiltPos);   // sync the now-active backend to its position
    oledDirty = true;
  }
  prefs.putInt("ctgt", ctrlTarget);
}
void setJoyEnabled(int v) { joyEnabled = constrain(v, 0, 1); prefs.putInt("joyen", joyEnabled); }
void setJoyMode(int v)    { joyMode    = constrain(v, 0, 1); prefs.putInt("joymd", joyMode); }
void setLxRelax(int v)    { lxRelax    = constrain(v, 0, 1); prefs.putInt("lxrelax", lxRelax); }

// Button remap (D-B, web-only). Each stores a button index 0..4 = A/B/C/D/OK.
// Duplicate assignments are allowed (one button then fires both actions) - the web page
// warns, the same way it does for an LX-16A pan/tilt ID collision.
void setMapHome(int v)  { mapHome  = constrain(v, 0, 4); prefs.putInt("kbhm", mapHome); }
void setMapBack(int v)  { mapBack  = constrain(v, 0, 4); prefs.putInt("kbbk", mapBack); }
void setMapEnter(int v) { mapEnter = constrain(v, 0, 4); prefs.putInt("kben", mapEnter); }
void setMapTgl(int v)   { mapTgl   = constrain(v, 0, 4); prefs.putInt("kbtg", mapTgl); }
void setPanId(int v)  { panid  = constrain(v, 0, 253); prefs.putInt("panid",  panid); }
void setTiltId(int v) { tiltid = constrain(v, 0, 253); prefs.putInt("tiltid", tiltid); }

void setPanMin(int v)  { panMin  = constrain(v, SERVO_MIN_DEG, panMax);  prefs.putInt(ckey("pmin", ctrlTarget), panMin);  reclampToLimits(); }
void setPanMax(int v)  { panMax  = constrain(v, panMin, SERVO_MAX_DEG);  prefs.putInt(ckey("pmax", ctrlTarget), panMax);  reclampToLimits(); }
void setTiltMin(int v) { tiltMin = constrain(v, SERVO_MIN_DEG, tiltMax); prefs.putInt(ckey("tmin", ctrlTarget), tiltMin); reclampToLimits(); }
void setTiltMax(int v) { tiltMax = constrain(v, tiltMin, SERVO_MAX_DEG); prefs.putInt(ckey("tmax", ctrlTarget), tiltMax); reclampToLimits(); }

void setHomePan(int v)  { homePan  = constrain(v, panMin,  panMax);   prefs.putInt(ckey("hpan",  ctrlTarget), homePan); }
void setHomeTilt(int v) { homeTilt = constrain(v, tiltMin, tiltMax);  prefs.putInt(ckey("htilt", ctrlTarget), homeTilt); }

// C,SH: capture the current live position as the home target (clamped to limits).
void captureHome() {
  homePan  = constrain(panPos,  panMin,  panMax);
  homeTilt = constrain(tiltPos, tiltMin, tiltMax);
  prefs.putInt(ckey("hpan",  ctrlTarget), homePan);
  prefs.putInt(ckey("htilt", ctrlTarget), homeTilt);
}

// C,RS: reset calibration (limits 0/180, home 90/90, speed 90). Persist all,
// then re-clamp live state. stepSize is NOT part of calibration (left as-is).
void resetCalibration() {
  panMin = SERVO_MIN_DEG; panMax = SERVO_MAX_DEG;
  tiltMin = SERVO_MIN_DEG; tiltMax = SERVO_MAX_DEG;
  homePan = HOME_DEG; homeTilt = HOME_DEG; homeSpeed = 90;
  // Resets the ACTIVE target's calibration only (each target owns its own - D-A2).
  prefs.putInt(ckey("pmin", ctrlTarget), panMin);
  prefs.putInt(ckey("pmax", ctrlTarget), panMax);
  prefs.putInt(ckey("tmin", ctrlTarget), tiltMin);
  prefs.putInt(ckey("tmax", ctrlTarget), tiltMax);
  prefs.putInt(ckey("hpan", ctrlTarget), homePan);
  prefs.putInt(ckey("htilt", ctrlTarget), homeTilt);
  prefs.putInt(ckey("hspd", ctrlTarget), homeSpeed);
  reclampToLimits();
}

// ============================================================
//  nudge(): move one axis by one step in direction dir (+1 / -1).
//    1. delta = dir * stepSize
//    2. if this axis's invert flag is set, negate delta
//    3. add to the stored position, clamp to [SERVO_MIN_DEG, SERVO_MAX_DEG]
//    4. write the clamped angle to that servo
//  Design notes:
//   - Clamp (constrain), not wrap: a servo at its limit holds instead of
//     slamming from one extreme to the other.
//   - Invert is applied by negating delta (not by remapping buttons), so the
//     command table stays fixed and a reversed servo is a one-line config flip.
//  v2 changes:
//   - A manual nudge cancels any in-progress home glide (manual override wins).
//   - Clamp to the axis's SOFT limits (panMin..panMax / tiltMin..tiltMax)
//     instead of the fixed 0..180.
// ============================================================
void nudge(Axis axis, int dir) {
  if (testBusy()) return;                   // range test owns the rig
  homing = false;                 // manual override: stop any home glide
  int delta = dir * stepSize;
  if (axis == PAN) {
    if (INVERT_PAN) delta = -delta;
    int before = panPos;
    panPos = constrain(panPos + delta, panMin, panMax);
    driveAngle(PAN, panPos);
    signalMove(PAN, panPos - before);      // corner LEDs follow the real movement
  } else {  // TILT
    if (INVERT_TILT) delta = -delta;
    int before = tiltPos;
    tiltPos = constrain(tiltPos + delta, tiltMin, tiltMax);
    driveAngle(TILT, tiltPos);
    signalMove(TILT, tiltPos - before);
  }
}

// ============================================================
//  Section 5 - WiFi state machine: STA first, AP fallback
// ============================================================
void connectWiFi() {
  // Credentials: use the ones provisioned from the web page (saved in NVS) if
  // present, else the compiled-in placeholders. Provisioning writes wssid/wpass
  // then reboots, so the new network is picked up here on the next boot.
  String provSsid = prefs.getString("wssid", "");
  String provPass = prefs.getString("wpass", "");
  const char* ssid = provSsid.length() ? provSsid.c_str() : HOME_SSID;
  const char* pass = provSsid.length() ? provPass.c_str() : HOME_PASS;

  Serial.println();
  Serial.print(F("Connecting to WiFi SSID: "));
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  uint32_t start = millis();
  bool ledOn = false;
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_STA_TIMEOUT_MS) {
    ledOn = !ledOn;
    setLed(ledOn);            // fast-blink while connecting
    Serial.print('.');
    delay(LED_FAST_MS);
  }

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    WiFi.setAutoReconnect(true);
    ip = WiFi.localIP();
    setLed(true);            // solid = STA
    Serial.println();
    Serial.print(F("STA connected. IP: "));
    Serial.println(ip);
  } else {
    // Hotspot fallback: PURE AP so it broadcasts reliably on one channel.
    // (Provisioning's /scan briefly enables STA just for the scan, then restores this.)
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    apMode = true;
    ip = WiFi.softAPIP();    // 192.168.4.1
    setLed(false);          // loop() drives the slow-blink from here
    Serial.println();
    Serial.print(F("STA timed out. Started AP '"));
    Serial.print(AP_SSID);
    Serial.print(F("'. IP: "));
    Serial.println(ip);
  }
}

// ============================================================
//  Section 6 - HTTP route handlers
// ============================================================

// GET /  -> serve the self-contained control page from PROGMEM.
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

// GET /action?go=<cmd>  -> run a command, reply with the unified state CSV (fullStatus).
// Protocol (all values clamped server-side; see spec section 6):
//   B,L/B,R  pan -/+ step    B,U/B,D tilt +/- step    B,H  start home glide
//   S,<n>    step size        V,<n>  home speed
//   C,SH     home = current   C,PL/C,PH/C,TL/C,TH  capture limit = current
//   C,RS     reset calibration
//   N,PL/N,PH/N,TL/N,TH,<v>   set that limit = v
//   N,HP/N,HT,<v>             set homePan / homeTilt = v
//   M,PI/M,TI,<id>            set LX-16A pan/tilt bus ID (persist + live rebind)
void handleAction() {
  if (!server.hasArg("go")) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(404, "text/plain", "missing go");
    return;
  }

  String go    = server.arg("go");
  String group = getValue(go, ',', 0);   // "B" / "S" / "V" / "C" / "N"
  String what  = getValue(go, ',', 1);   // sub-command or number
  int    val   = getValue(go, ',', 2).toInt();   // only used by N,*,<v>

  if (group == "B") {
    if      (what == "L") nudge(PAN,  -1);   // pan left
    else if (what == "R") nudge(PAN,  +1);   // pan right
    else if (what == "U") nudge(TILT, +1);   // tilt up
    else if (what == "D") nudge(TILT, -1);   // tilt down
    else if (what == "H") startHome();       // smooth glide to home
  } else if (group == "S") {
    setStep(what.toInt());                   // step size (clamped + saved)
  } else if (group == "V") {
    setHomeSpeed(what.toInt());              // home glide speed (clamped + saved)
  } else if (group == "C") {                 // capture-current / commands
    if      (what == "SH") captureHome();
    else if (what == "PL") setPanMin(panPos);
    else if (what == "PH") setPanMax(panPos);
    else if (what == "TL") setTiltMin(tiltPos);
    else if (what == "TH") setTiltMax(tiltPos);
    else if (what == "RS") resetCalibration();
  } else if (group == "N") {                 // numeric setters: N,<what>,<v>
    if      (what == "PL") setPanMin(val);
    else if (what == "PH") setPanMax(val);
    else if (what == "TL") setTiltMin(val);
    else if (what == "TH") setTiltMax(val);
    else if (what == "HP") setHomePan(val);
    else if (what == "HT") setHomeTilt(val);
  } else if (group == "M") {                 // LX-16A live ID link (section 6.4): M,<what>,<id>
    if      (what == "PI") { setPanId(val);  backend->bind(PAN,  panid); }   // pan  ID: persist (0-253) + rebind
    else if (what == "TI") { setTiltId(val); backend->bind(TILT, tiltid); }  // tilt ID: persist (0-253) + rebind
  } else if (group == "T") {                 // control-target router: T,<0=PWM|1=LX|2=Both> (live)
    setCtrlTarget(what.toInt());
  } else if (group == "J") {                 // joystick enable: J,<0|1>
    setJoyEnabled(what.toInt());
  } else if (group == "Y") {                 // joystick mode: Y,<0=RATE|1=ABSOLUTE>
    setJoyMode(what.toInt());
  } else if (group == "R") {                 // idle torque release: R,<0=hold|1=release after 3s>
    setLxRelax(what.toInt());
  } else if (group == "K") {                 // button remap (D-B, web-only): K,<HM|BK|EN|TG>,<0..4>
    if      (what == "HM") setMapHome(val);
    else if (what == "BK") setMapBack(val);
    else if (what == "EN") setMapEnter(val);
    else if (what == "TG") setMapTgl(val);
  }
  // Unknown group/cmd: no-op, still report current state.

  oledDirty = true;    // state may have changed -> refresh OLED next loop

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", fullStatus());
}

// GET /status  -> the same unified state CSV as /action (see fullStatus()).
// Polled ~every 700 ms by the page so it reflects the autonomous home glide.
void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", fullStatus());
}

// Unknown route (or missing go handled above) -> 404.
void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ============================================================
//  WiFi provisioning (scan / save+join / forget). Blocking scan is fine here;
//  it is a manual, occasional action.
// ============================================================

// GET /scan  -> one network per line: "ssid<TAB>rssi<TAB>locked(0/1)".
// TAB-separated because SSIDs may contain commas.
void handleScan() {
  // In hotspot mode the station radio is off; enable it just for the scan so
  // WiFi.scanNetworks() works, then restore AP-only so the hotspot stays up.
  bool apOnly = (WiFi.getMode() == WIFI_AP);
  if (apOnly) WiFi.mode(WIFI_AP_STA);

  int n = WiFi.scanNetworks();
  String out;
  out.reserve(n > 0 ? n * 24 : 0);
  for (int i = 0; i < n; i++) {
    if (i) out += '\n';
    out += WiFi.SSID(i);
    out += '\t';
    out += WiFi.RSSI(i);
    out += '\t';
    out += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? '0' : '1';
  }
  WiFi.scanDelete();
  if (apOnly) WiFi.mode(WIFI_AP);       // back to stable AP-only

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", out);
}

// GET /setwifi?ssid=..&pass=..  -> save creds to NVS and reboot to join.
// pass may be empty (open network). Values arrive URL-decoded from the page.
void handleSetWifi() {
  if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(400, "text/plain", "missing ssid");
    return;
  }
  prefs.putString("wssid", server.arg("ssid"));
  prefs.putString("wpass", server.hasArg("pass") ? server.arg("pass") : String(""));
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "saved");
  Serial.print(F("WiFi provisioned for SSID: "));
  Serial.println(server.arg("ssid"));
  delay(400);                 // let the response flush before we reboot
  ESP.restart();
}

// GET /forgetwifi  -> clear saved creds and reboot (back to placeholders -> AP).
void handleForgetWifi() {
  prefs.remove("wssid");
  prefs.remove("wpass");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "forgotten");
  Serial.println(F("WiFi credentials cleared"));
  delay(400);
  ESP.restart();
}

// ============================================================
//  Section 6.5 - Servo-backend endpoints (LX-16A serial bus)
//  Plain-text TSV / key=value (no JSON), all HTTP_GET + CORS. The three
//  bus-touching endpoints refuse in PWM mode (the bus is only opened when
//  bknd==1); /setbackend is exempt (it just writes NVS + reboots). Exact wire
//  formats (the web UI must match these byte-for-byte):
//    /servoscan?mode=range&hi=N  -> 0+ lines "id<TAB>vinMv<TAB>tempC<TAB>posDeg"
//    /servoscan?mode=lone        -> one line "<id>"   (0 = none / ambiguous)
//    /servoid?new=&confirm=1[&cur=]  -> "ok<TAB><id>" | "fail<TAB><reason>"
//    /servocfg?id=                   -> "key=value" lines (one register per line)
//    /servocfg?id=&set=&v=[&v2=]      -> "ok" | "fail<TAB><reason>"
//    /setbackend?b=0|1               -> "saved" then reboot
// ============================================================

// Shared CORS header + LX-active guard for the three bus endpoints. Sends the
// refusal and returns false when the bus is not live, so callers early-return.
// (Audit: these endpoints crashed on the never-initialized library bus object;
// the library objects no longer exist - everything runs on the half-duplex
// engine, and lxBusBegin() here makes the bus state explicit, never assumed.)
bool servoBusReady() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (bknd != 1) { server.send(200, "text/plain", "fail\tpwm_mode"); return false; }
  if (!lxBusUp) lxBusBegin();
  return true;
}

// Sweep window for broadcast diagnostics: stay inside BOTH axes' soft-limit windows of
// the LX TARGET'S OWN slot (review finding: using the live globals meant the sweep was
// clamped by whatever target happened to be active - possibly the PWM slot's wide-open
// window - and could drive the LX rig past its calibrated stops). saveSlot() first so
// the slot array reflects live values when the LX target IS the active one.
static void lxSweepWindow(int &a, int &b, int &mid) {
  saveSlot(ctrlTarget);
  int lo = max(ctrlSlot[1].panMin, ctrlSlot[1].tiltMin);
  int hi = min(ctrlSlot[1].panMax, ctrlSlot[1].tiltMax);
  if (lo > hi) { int t = lo; lo = hi; hi = t; }     // disjoint windows: use the overlap edges
  mid = constrain(90, lo, hi);
  a   = constrain(mid - 20, lo, hi);
  b   = constrain(mid + 20, lo, hi);
}

// GET /lxprobe[?hi=N]  -> read-only bus scan (default IDs 1..12), works in BOTH
// backends with no reboot and no motion: brings the half-duplex engine up on
// demand and polls with reads only (no torque, no EEPROM writes, no moves).
void handleLxProbe() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!lxBusUp) lxBusBegin();
  int hi = server.hasArg("hi") ? constrain(server.arg("hi").toInt(), 1, 253) : 12;
  LxScanResult res[32];
  int n = lxScanRange(hi, res, 32);
  String out;
  out += "pin\t";   out += lxpin; out += '\n';
  out += "found\t"; out += n;     out += '\n';
  for (int i = 0; i < n; i++) {         // id, vin mV, temp C, pos deg
    out += res[i].id;     out += '\t';
    out += res[i].vinMv;  out += '\t';
    out += res[i].tempC;  out += '\t';
    out += res[i].posDeg; out += '\n';
  }
  server.send(200, "text/plain", out);
}

// GET /lxwiggle  -> BROADCAST move test (ID 254): ID-agnostic, works in both
// backends. Torque on, then a small staggered sweep CLAMPED inside the soft-limit
// windows (audit: the old fixed 60/120 sweep bypassed soft limits on a mounted
// rig, and blocking ~2.5s starved loop()). Total blocking ~1.3s - acceptable for
// an explicit manual diagnostic; the anti-snap guard in updateHoming covers it.
void handleLxWiggle() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!lxBusUp) lxBusBegin();
  uint8_t on[1] = { 1 };
  lxRawSend(254, LX16A_SERVO_LOAD_OR_UNLOAD_WRITE, on, 1);   // broadcast torque ON
  delay(50);
  int a, b, mid;
  lxSweepWindow(a, b, mid);
  lxRawMove(254, a,   400); delay(450);
  lxRawMove(254, b,   400); delay(450);
  lxRawMove(254, mid, 400); delay(400);
  String o = "broadcast sent: torque ON + sweep ";
  o += a; o += "->"; o += b; o += "->"; o += mid; o += " deg to ID 254";
  server.send(200, "text/plain", o);
}

// GET /lxtx  -> is the ESP32 actually putting bytes on the bus wire? The
// half-duplex engine's RX is permanently attached to the pin, so it HEARS OUR
// OWN TX ECHO - counting echo bytes is direct wire-level proof, with none of the
// old digitalRead sampling (whose input buffer was disabled -> false negatives,
// audit finding). No pinMode anywhere (it would detach the RX routing).
void handleLxTx() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!lxBusUp) lxBusBegin();

  // Hand-rolled send WITHOUT the echo discard, so the echo becomes the evidence.
  uint8_t buf[10] = { 0x55, 0x55, 254, 3, LX16A_SERVO_ID_READ, 0 };
  buf[5] = (uint8_t)~(uint8_t)(254 + 3 + LX16A_SERVO_ID_READ);
  while (Serial2.available()) Serial2.read();
  lxTxAttach();
  Serial2.write(buf, 6);
  Serial2.flush();
  lxTxRelease();
  int echoed = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 6) { if (Serial2.available()) { Serial2.read(); echoed++; } }

  String o;
  o += "pin\t";     o += lxpin;  o += '\n';
  o += "sent\t6\n";
  o += "echoed\t";  o += echoed; o += '\n';
  o += "verdict\t";
  if (echoed >= 6)      o += "TX proven on the wire (RX heard our own bytes)";
  else if (echoed > 0)  o += "partial echo - wire driven but lossy (check wiring quality)";
  else                  o += "no echo: TX not reaching the wire, or RX routing lost - POST /lxfix to re-arm";
  server.send(200, "text/plain", o);
}

// GET /lxfix  -> recovery + self-test: force a full half-duplex re-init, prove the
// wire with the echo test, then broadcast torque-on + a small clamped sweep.
void handleLxFix() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  lxBusBegin();                                   // unconditional re-arm

  uint8_t on[1] = { 1 };
  lxRawSend(254, LX16A_SERVO_LOAD_OR_UNLOAD_WRITE, on, 1);
  delay(60);
  int a, b, mid;
  lxSweepWindow(a, b, mid);
  lxRawMove(254, a,   400); delay(450);
  lxRawMove(254, b,   400); delay(450);
  lxRawMove(254, mid, 400); delay(400);

  String o = "half-duplex engine re-armed on GPIO ";
  o += lxpin;
  o += "; torque ON + sweep ";
  o += a; o += "->"; o += b; o += "->"; o += mid;
  o += " sent (broadcast)";
  server.send(200, "text/plain", o);
}

// GET /lxpintest  -> electrical sanity check on the bus pin (driven high / tied
// to ground / free). The pinMode probes detach the pin from the UART through the
// peripheral manager, so the bus engine is ALWAYS fully re-initialized afterwards
// (audit: the old restore re-entered the library's one-pin mode, killing raw
// motion and potentially leaving the pad driving the bus LOW).
void handleLxPinTest() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  int p = lxpin;
  pinMode(p, INPUT);          delay(2); int fl = digitalRead(p);   // floating
  pinMode(p, INPUT_PULLDOWN); delay(5); int dn = digitalRead(p);   // fights an external pull-up
  pinMode(p, INPUT_PULLUP);   delay(5); int up = digitalRead(p);   // fights an external pull-down
  lxBusBegin();                                                    // full engine re-init
  String out;
  out += "pin\t";      out += p;  out += '\n';
  out += "float\t";    out += fl; out += '\n';
  out += "pulldown\t"; out += dn; out += '\n';
  out += "pullup\t";   out += up; out += '\n';
  out += "verdict\t";
  if (dn == 1)                 out += "HIGH against our pulldown -> line is driven/pulled high (powered servo present)";
  else if (up == 0)            out += "LOW against our pullup -> line is tied to GND: signal wire on the wrong pin, or an unpowered servo clamping it";
  else if (fl == 0 && up == 1) out += "line is free -> nothing connected/driving it";
  else                         out += "inconclusive";
  server.send(200, "text/plain", out);
}

// GET /servoscan?mode=range|lone  -> bus discovery (section 6.1).
void handleServoScan() {
  if (!servoBusReady()) return;
  String mode = server.hasArg("mode") ? server.arg("mode") : "range";
  if (mode == "lone") {
    String out; out += lxScanLone();            // single servo's ID, or 0 = none/ambiguous
    server.send(200, "text/plain", out);
    return;
  }
  int hi = server.hasArg("hi") ? server.arg("hi").toInt() : 30;
  LxScanResult res[32];
  int n = lxScanRange(hi, res, 32);
  String out; out.reserve(n * 20 + 4);
  for (int i = 0; i < n; i++) {
    if (i) out += '\n';
    out += res[i].id;    out += '\t';
    out += res[i].vinMv; out += '\t';
    out += res[i].tempC; out += '\t';
    out += res[i].posDeg;
  }
  server.send(200, "text/plain", out);          // may be empty (no responders)
}

// GET /servoid?new=<0-253>&confirm=1[&cur=<id>]  -> safe ID programming (section 6.2).
// No cur   -> broadcast flow A (requires exactly one servo, authenticated via mode=lone).
// cur given-> targeted flow B (raw write + duplicate pre-check). Both verify-after-write.
void handleServoId() {
  if (!servoBusReady()) return;
  if (!server.hasArg("new"))        { server.send(200, "text/plain", "fail\tbad_id");       return; }
  if (server.arg("confirm") != "1") { server.send(200, "text/plain", "fail\tneed_confirm"); return; }
  int newId = server.arg("new").toInt();
  LxIdResult r = server.hasArg("cur")
               ? lxProgramIdTargeted(server.arg("cur").toInt(), newId)
               : lxProgramIdBroadcast(newId);
  String out;
  if (r.ok) { out = "ok\t";   out += r.id;  }
  else      { out = "fail\t"; out += r.msg; }
  server.send(200, "text/plain", out);
}

// GET /servocfg?id=<id>[&set=<field>&v=<n>[&v2=<n>]]  -> per-servo config (section 6.3).
// No set -> read every register as key=value lines. With set -> one server-validated
// write (field in alim/vlim/tlim/mode/torque/trim/trimsave/led/lederr/factory).
void handleServoCfg() {
  if (!servoBusReady()) return;
  if (!server.hasArg("id")) { server.send(200, "text/plain", "fail\tno_id"); return; }
  int id = server.arg("id").toInt();

  if (server.hasArg("set")) {                   // ---- write one field ----
    String field = server.arg("set");
    int v  = server.hasArg("v")  ? server.arg("v").toInt()  : 0;
    int v2 = server.hasArg("v2") ? server.arg("v2").toInt() : 0;
    LxWriteResult w = lxCfgWrite(id, field.c_str(), v, v2);
    if (w.ok) server.send(200, "text/plain", "ok");
    else    { String out = "fail\t"; out += w.msg; server.send(200, "text/plain", out); }
    return;
  }

  LxConfig c = lxCfgRead(id);                   // ---- read all registers ----
  String out; out.reserve(224);
  out += "id=";       out += c.id;
  out += "\nalim=";   out += c.alimMin; out += ','; out += c.alimMax;
  out += "\nvlim=";   out += c.vlimMin; out += ','; out += c.vlimMax;
  out += "\ntlim=";   out += c.tlim;
  out += "\nmode=";   out += c.mode;    out += ','; out += c.speed;
  out += "\ntorque="; out += c.torque;
  out += "\ntrim=";   out += c.trim;
  out += "\nled=";    out += c.led;
  out += "\nlederr="; out += c.lederr;
  out += "\npos=";    out += c.posDeg;
  out += "\nvin=";    out += c.vinMv;
  out += "\ntemp=";   out += c.tempC;
  server.send(200, "text/plain", out);
}

// GET /setbackend?b=<0|1>  -> persist bknd, then reboot (mirrors /setwifi; switching
// backends changes pin/peripheral usage, so a clean re-init is safest). Section 7.3.
void handleSetBackend() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("b")) { server.send(400, "text/plain", "missing b"); return; }
  setBknd(server.arg("b").toInt());
  server.send(200, "text/plain", "saved");
  Serial.printf("Backend set to %d (%s), restarting...\n", bknd, bknd ? "LX16A" : "PWM");
  delay(400);                 // let the response flush before we reboot
  ESP.restart();
}

// GET /setpwmpins?pan=<gpio>&tilt=<gpio>  -> validate both, persist, reboot onto the new
// PWM pins. Same persist+reboot rationale as /setlxpin; both args required so a collision
// between the pair is always checked as a pair.
void handleSetPwmPins() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("pan") || !server.hasArg("tilt")) { server.send(400, "text/plain", "missing pan/tilt"); return; }
  int pp = server.arg("pan").toInt();
  int tp = server.arg("tilt").toInt();
  const char* bad = pwmPinReject(pp, tp);
  if (!bad && pp == tp) bad = "pan and tilt must differ";
  if (!bad) bad = pwmPinReject(tp, pp);
  if (bad) { server.send(400, "text/plain", String("fail\t") + bad); return; }
  pwmPanPin = pp;  prefs.putInt("ppin", pwmPanPin);
  pwmTiltPin = tp; prefs.putInt("tpin", pwmTiltPin);
  server.send(200, "text/plain", "saved");
  Serial.printf("PWM pins set to pan=%d tilt=%d, restarting...\n", pwmPanPin, pwmTiltPin);
  delay(400);
  ESP.restart();
}

// GET /setlxpin?pin=<gpio>  -> validate, persist, reboot onto the new LX bus pin.
// Rebooting (rather than re-opening Serial2 live) keeps bring-up deterministic - the
// same reasoning as /setbackend and WiFi provisioning. Rejects unusable pins with a
// reason, so a bad choice can never strand the bus or fight another peripheral.
void handleSetLxPin() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("pin")) { server.send(400, "text/plain", "missing pin"); return; }
  int p = server.arg("pin").toInt();
  const char* bad = lxPinReject(p);
  if (bad) { server.send(400, "text/plain", String("fail\t") + bad); return; }
  lxpin = p;
  prefs.putInt("lxpin", lxpin);
  server.send(200, "text/plain", "saved");
  Serial.printf("LX bus pin set to GPIO %d, restarting...\n", lxpin);
  delay(400);
  ESP.restart();
}

// ============================================================
//  Section 9.2/9.3 - LX-16A telemetry + firmware-derived fault
// ============================================================

// deriveFault(): compute the fault bitmask from cached telemetry (the LX-16A has NO
// live-fault register - cmd 36 is alarm CONFIG, not status). Bits 0-3 = pan, 4-7 =
// tilt: bit0 temp>=cap, bit1 vin out-of-window, bit2 |cmd-actual|>STALL, bit3 bus-dead.
// A field with no valid sample (-1) never raises its bit; bits clear on recovery.
int deriveFault() {
  int f = 0;
  if (telPanTemp    >= 0 && telPanTemp  >= LX_TEMP_CAP_C)                                f |= 0x01;
  if (telPanVin     >= 0 && (telPanVin  < LX_VIN_MIN_MV || telPanVin  > LX_VIN_MAX_MV))  f |= 0x02;
  if (ctrlTarget != 0 && !lxRelaxed[0] &&            // stall is only meaningful when the live
      telPanActual  >= 0 && abs(panPos  - telPanActual)  > STALL_DEG)  f |= 0x04;   // pos commands the LX rig
  if (panSeen  && panFailN  >= FAULT_FAIL_N)                                             f |= 0x08;
  if (telTiltTemp   >= 0 && telTiltTemp >= LX_TEMP_CAP_C)                                f |= 0x10;
  if (telTiltVin    >= 0 && (telTiltVin < LX_VIN_MIN_MV || telTiltVin > LX_VIN_MAX_MV))  f |= 0x20;
  if (ctrlTarget != 0 && !lxRelaxed[1] &&
      telTiltActual >= 0 && abs(tiltPos - telTiltActual) > STALL_DEG)  f |= 0x40;
  if (tiltSeen && tiltFailN >= FAULT_FAIL_N)                                             f |= 0x80;
  return f;
}

// updateTelemetry(): throttled (~TELEM_MS), LX-16A-only, idle-only round-robin bus
// reads (section 9.2). Position is read every tick (headline commanded-vs-actual); the
// slower vin/temp rotate one per tick. Reads are best-effort - the backend returns -1
// when !isCommandOk(), which we discard (keep the last good sample) while counting
// toward the consecutive-failure bus-dead latch. NEVER runs during/just-after motion.
void updateTelemetry() {
  if (!backend || !backend->telemetry()) return;   // PWM: no bus (bknd != 1) -> leaves fields -1
  if (millis() < ledActiveUntil)         return;   // skip blocking reads during/just-after motion
  if (millis() - telemLastMs < TELEM_MS) return;
  telemLastMs = millis();

  int pa = backend->readAngle(PAN);
  if (pa >= 0) { telPanActual  = pa; panFailN  = 0; panSeen  = true; } else if (panFailN  < 250) panFailN++;
  int ta = backend->readAngle(TILT);
  if (ta >= 0) { telTiltActual = ta; tiltFailN = 0; tiltSeen = true; } else if (tiltFailN < 250) tiltFailN++;

  switch (telemStep++ & 3) {                        // one slower voltage/temperature read per tick
    case 0: { int v = backend->readVinMv(PAN);  if (v >= 0) telPanVin   = v; break; }
    case 1: { int t = backend->readTempC(PAN);  if (t >= 0) telPanTemp  = t; break; }
    case 2: { int v = backend->readVinMv(TILT); if (v >= 0) telTiltVin  = v; break; }
    case 3: { int t = backend->readTempC(TILT); if (t >= 0) telTiltTemp = t; break; }
  }

  servoFault = deriveFault();                        // faults computed in firmware, not a servo register
}

// ============================================================
//  Section 9.4 - stiction auto-trim driver (the user's staircase idea):
//  command -> settle -> measure shortfall -> bump past the target -> when it
//  finally lands, learn the total overshoot needed for next time. Runs on its
//  own 400ms cadence (one cheap position read) so a correction converges in
//  ~1s instead of waiting for the 1.5s telemetry tick. Per-axis, direction-aware.
// ============================================================
uint32_t trimLastMs        = 0;
int      trimPrevTarget[2] = { -999, -999 };   // logical target the staircase is working on
uint8_t  trimBudget[2]     = { 0, 0 };         // remaining bumps for this target (2 = user's staircase)
bool     trimDone[2]       = { false, false }; // learned/recorded for this target
int      trimPersist10[4]  = { 0, 0, 0, 0 };   // last NVS-persisted values (throttle writes)

void updateLxTrim() {
  if (testBusy()) return;                   // never trim-correct a range test
  if (bknd != 1 || !lxBusUp || homing) return;
  if (ctrlTarget == 0) return;                       // LX rig not being driven
  uint32_t now = millis();
  if (now - trimLastMs < 400) return;
  trimLastMs = now;

  for (int i = 0; i < 2; i++) {
    Axis a      = (i == 0) ? PAN : TILT;
    int logical = (i == 0) ? panPos : tiltPos;

    if (trimPrevTarget[i] != logical) {              // new target -> fresh staircase
      trimPrevTarget[i] = logical;
      trimBudget[i]     = 2;
      trimDone[i]       = false;
    }
    // Parked (settled, or never commanded beyond begin()'s glide): optionally release
    // torque after 3s of quiet - the hold-controller stops stall-pushing/buzzing.
    // NOTE: lxPhys < 0 must never reach the learner below - the -1 sentinel once got
    // treated as a position and slammed the trim to its cap.
    if (trimDone[i] || lxB.lxPhys[i] < 0) {
      if (lxRelax && !lxRelaxed[i] && now - lxB.lxCmdMs[i] > 3000) lxB.relax(a);
      continue;
    }
    if (now - lxB.lxCmdMs[i] < 450) continue;        // let the move finish + mechanics settle

    int actual = backend->readAngle(a);
    if (actual < 0) continue;                        // no servo / no reply: try next tick
    int err = logical - actual;                      // + = landed short in the up direction

    if (abs(err) <= 1) {                             // ARRIVED: learn what it took, record
      lxTrimLearn(i, lxB.lxDir[i], abs(lxB.lxPhys[i] - logical) * 10);
      lxCalRecord(i, lxB.lxDir[i], logical, lxB.lxPhys[i], actual);
      trimDone[i] = true;
    } else if (trimBudget[i] > 0) {                  // staircase: push by the shortfall
      lxCalRecord(i, lxB.lxDir[i], logical, lxB.lxPhys[i], actual);
      lxB.bumpPhys(a, err);
      trimBudget[i]--;
    } else {                                         // budget spent: record the residual miss
      lxCalRecord(i, lxB.lxDir[i], logical, lxB.lxPhys[i], actual);
      // Adopt only when the LX rig is the SOLE target (review finding: in "Both" mode
      // panPos/tiltPos also command the mirrored PWM rig, which was never re-driven -
      // adopting the LX servo's reality silently desynced the PWM servo from the UI).
      if (ctrlTarget == 1 && abs(err) <= 2) {        // user rule: within 2 deg, accept the servo's
        int adopt = constrain(actual,                // reported position as the truth (clamped to
                              (i == 0) ? panMin : tiltMin,     // the soft-limit window)...
                              (i == 0) ? panMax : tiltMax);
        if (i == 0) panPos = adopt; else tiltPos = adopt;
        lxB.parkAt(a, adopt);                        // ...and re-command AT it so the controller
        trimPrevTarget[i] = adopt;                   // rests instead of leaning on the gap forever
        oledDirty = true;
      }
      trimDone[i] = true;
    }
  }

  // Persist learned trims when they move >=0.2 deg from what NVS holds (wear-friendly).
  int cur[4] = { lxTrim10Up[0], lxTrim10Dn[0], lxTrim10Up[1], lxTrim10Dn[1] };
  const char* keys[4] = { "cbu0", "cbd0", "cbu1", "cbd1" };
  for (int k = 0; k < 4; k++) {
    if (abs(cur[k] - trimPersist10[k]) >= 2) {
      prefs.putInt(keys[k], cur[k]);
      trimPersist10[k] = cur[k];
    }
  }
}

// Write one raw step of the range test to the servo under test. Direct backend
// writes on purpose: driveAngle() routes/clamps by control target, and the whole
// point here is a full-travel sweep of ONE specific servo.
void testWrite(int deg) {
  if      (testKind == 0) pwmB.writeAngle(PAN,  deg);
  else if (testKind == 1) pwmB.writeAngle(TILT, deg);
  else if (testKind == 2) lxRawMove((uint8_t)testLxId, deg, 150);
}

// Advance the range test one tick (called from loop; no-op when idle/halted).
void updateServoTest() {
  if (testKind < 0 || testHalted) return;
  uint32_t now = millis();

  // Collision watch (LX only): commanded advances, measured position stops following.
  if (testKind == 2 && now - testChkMs >= TEST_CHK_MS) {
    testChkMs = now;
    int act = lxTicksToDeg(lxRead16((uint8_t)testLxId, LX16A_SERVO_POS_READ));
    if (act >= 0) {
      if (abs(testDeg - act) > TEST_ERR_DEG &&
          testPrevActual >= 0 && abs(act - testPrevActual) < 2) {
        if (++testStallN >= 2) {                       // ~1s of no progress with a real gap
          int backoff = constrain(act - 5 * testDir, 0, 180);
          lxRawMove((uint8_t)testLxId, backoff, 250);  // back off 5 deg opposite of travel, asap
          testHalted = true;
          snprintf(testMsg, sizeof(testMsg),
                   "COLLISION at %d deg (cmd %d) - backed off to %d, HALTED", act, testDeg, backoff);
          Serial.println(testMsg);
          return;
        }
      } else testStallN = 0;
      testPrevActual = act;
      testActual = act;
    }
  }

  if (now - testStepMs < TEST_STEP_MS) return;
  testStepMs = now;

  int target = (testPhase == 0) ? 0 : (testPhase == 1) ? 180 : testPark;
  if (testDeg == target) {
    if (testPhase >= 2) {                              // completed the full pattern
      snprintf(testMsg, sizeof(testMsg), "complete - full 0-180 travel OK, parked at %d", testPark);
      testKind = -1;
      return;
    }
    testPhase++;
    return;
  }
  testDir  = (target > testDeg) ? 1 : -1;
  testDeg += testDir;
  testWrite(testDeg);
}

// GET /servotest            -> status (state/kind/cmd/actual/msg)
//     /servotest?start=pwm&axis=pan|tilt
//     /servotest?start=lx&id=<n>
//     /servotest?stop=1     -> stop / clear a halt (regains normal control)
void handleServoTest() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("stop")) {
    if (testKind >= 0 || testHalted) snprintf(testMsg, sizeof(testMsg), "stopped by user");
    testKind = -1; testHalted = false;
    server.send(200, "text/plain", "stopped");
    return;
  }
  if (server.hasArg("start")) {
    if (testBusy()) { server.send(200, "text/plain", "fail\talready_running"); return; }
    String what = server.arg("start");
    testHalted = false; testStallN = 0; testPrevActual = -1; testActual = -1;
    testPhase = 0; testStepMs = millis(); testChkMs = millis();
    if (what == "pwm") {
      testKind = (server.arg("axis") == "tilt") ? 1 : 0;
      testPark = (testKind == 0) ? ctrlSlot[0].pan : ctrlSlot[0].tilt;
      testDeg  = testPark;                             // start from where the rig believes it is
      snprintf(testMsg, sizeof(testMsg), "PWM %s sweep (no feedback - watch it!)",
               testKind == 0 ? "pan" : "tilt");
    } else if (what == "lx") {
      if (bknd != 1) { server.send(200, "text/plain", "fail\tpwm_mode"); return; }
      if (!lxBusUp) lxBusBegin();
      testKind = 2;
      testLxId = constrain(server.arg("id").toInt(), 0, 253);
      int act = lxTicksToDeg(lxRead16((uint8_t)testLxId, LX16A_SERVO_POS_READ));
      if (act < 0) { testKind = -1; server.send(200, "text/plain", "fail\tno_reply"); return; }
      uint8_t on[1] = { 1 };                           // make sure it will actually move
      lxRawSend((uint8_t)testLxId, LX16A_SERVO_LOAD_OR_UNLOAD_WRITE, on, 1);
      if (testLxId == panid)       testPark = ctrlSlot[1].pan;
      else if (testLxId == tiltid) testPark = ctrlSlot[1].tilt;
      else                         testPark = 90;
      testDeg = act;                                   // start from the servo's REAL position
      testActual = testPrevActual = act;
      snprintf(testMsg, sizeof(testMsg), "LX id %d sweep with collision watch", testLxId);
    } else { server.send(200, "text/plain", "fail\tbad_start"); return; }
    server.send(200, "text/plain", "started");
    return;
  }
  String o;
  o += "state\t"; o += testHalted ? "HALTED" : (testKind >= 0 ? "running" : "idle"); o += '\n';
  o += "kind\t";  o += (testKind == 0) ? "pwm-pan" : (testKind == 1) ? "pwm-tilt"
                      : (testKind == 2) ? "lx" : "-"; o += '\n';
  o += "id\t";    o += testLxId;  o += '\n';
  o += "cmd\t";   o += testDeg;   o += '\n';
  o += "actual\t"; o += testActual; o += '\n';
  o += "msg\t";   o += testMsg;   o += '\n';
  server.send(200, "text/plain", o);
}

// GET /lxcal[?reset=1]  -> the auto-trim's learned overshoots + the last 10 settle
// records (newest first), so "how much does this servo need" is inspectable data.
void handleLxCal() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("reset")) {
    lxTrim10Up[0] = lxTrim10Dn[0] = lxTrim10Up[1] = lxTrim10Dn[1] = 0;
    prefs.putInt("cbu0", 0); prefs.putInt("cbd0", 0);
    prefs.putInt("cbu1", 0); prefs.putInt("cbd1", 0);
    trimPersist10[0] = trimPersist10[1] = trimPersist10[2] = trimPersist10[3] = 0;
    lxCalHead = lxCalCount = 0;
    server.send(200, "text/plain", "trim + records reset");
    return;
  }
  String o;
  o += "learned trim (deg, applied to the FIRST command of a move):\n";
  o += "pan  up\t";  o += lxTrim10Up[0] / 10.0f; o += "\npan  down\t"; o += lxTrim10Dn[0] / 10.0f;
  o += "\ntilt up\t"; o += lxTrim10Up[1] / 10.0f; o += "\ntilt down\t"; o += lxTrim10Dn[1] / 10.0f;
  o += "\n\nrecent settles, newest first (axis dir cmd phys actual err age_s):\n";
  for (int k = 0; k < lxCalCount; k++) {
    int idx = (lxCalHead - 1 - k + 20) % 10;
    LxCalRec &r = lxCalRing[idx];
    o += (r.axis == 0) ? "pan " : "tilt";
    o += (r.dir >= 0) ? " +" : " -";
    o += '\t'; o += r.cmd;    o += '\t'; o += r.phys;
    o += '\t'; o += r.actual; o += '\t'; o += r.err;
    o += '\t'; o += (millis() - r.ms) / 1000; o += '\n';
  }
  server.send(200, "text/plain", o);
}

// ============================================================
//  Section 7 - OLED: detect the panel, then draw 3 cycling pages
// ============================================================

// Does a device ACK at this 7-bit address on the I2C bus?
bool i2cAck(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Bring up I2C on (sda,scl) and see if an SH1107 answers at 0x3C or 0x3D.
// On success, initialise the panel and return true.
bool oledTry(int sda, int scl) {
  Wire.begin(sda, scl);
  Wire.setClock(400000);
  if      (i2cAck(0x3C)) oledAddr = 0x3C;
  else if (i2cAck(0x3D)) oledAddr = 0x3D;
  else return false;
  oledPresent = true;
  oled.setI2CAddress(oledAddr << 1);   // U8g2 wants the 8-bit address form
  oled.begin();                        // reuses the Wire pins just set
  oled.setBusClock(400000);
  return true;
}

// Find the panel: try the configured pins, then the swapped order (SDA/SCL are
// easy to mix up). Show a splash if found; else log once and run without it.
void setupOLED() {
  if (!oledTry(PIN_OLED_SDA, PIN_OLED_SCL) && oledTry(PIN_OLED_SCL, PIN_OLED_SDA)) {
    Serial.println(F("(OLED found with SDA/SCL swapped from the configured pins)"));
  }
  if (!oledPresent) {
    Serial.printf("OLED not found on GPIO %d/%d either order (0x3C/0x3D) - skipping display\n",
                  PIN_OLED_SDA, PIN_OLED_SCL);
    return;
  }

  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13B_tr);
  oled.drawStr(0, 40, "ESP32 Pan/Tilt");
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 60, "starting...");
  oled.sendBuffer();

  Serial.printf("OLED OK at 0x%02X\n", oledAddr);
}

// A 0..180 bar: border + fill, the soft-limit window (ticks at lo & hi that
// overhang the bar) and a home marker (small disc centered under the bar).
void oledBar(int x, int y, int w, int h, int value, int lo, int hi, int hm) {
  int inner = w - 2;
  oled.drawFrame(x, y, w, h);
  int fill = (int)((long)value * inner / (SERVO_MAX_DEG - SERVO_MIN_DEG));
  if (fill > 0) oled.drawBox(x + 1, y + 1, fill, h - 2);
  int lx = x + 1 + (int)((long)lo * inner / 180);   // soft-limit min tick
  int hx = x + 1 + (int)((long)hi * inner / 180);   // soft-limit max tick
  oled.drawVLine(lx, y - 2, h + 4);
  oled.drawVLine(hx, y - 2, h + 4);
  int mx = x + 1 + (int)((long)hm * inner / 180);   // home marker
  oled.drawDisc(mx, y + h + 2, 1);
}

// Three page-indicator dots along the bottom; the active one is filled.
void oledDots(uint8_t active) {
  for (uint8_t i = 0; i < oledPageCount; i++) {   // 3 (PWM) or 4 (LX-16A adds Servo Bus)
    int cx = 52 + i * 12;
    if (i == active) oled.drawDisc(cx, 124, 2);
    else             oled.drawCircle(cx, 124, 2);
  }
}

// ---- Button-press feedback (user request): the hint bar flashes the pressed button's
// ---- hint inverted (white box, black text) for ~400ms, on every page, so a click is
// ---- visibly acknowledged even when its effect isn't obvious. Declared ABOVE all the
// ---- page renderers that use it (structs/globals don't get Arduino auto-prototypes).
int      fbBtn = -1;          // JOY_BTN index (0..4) of the last dispatched click
uint32_t fbMs  = 0;

void hintBar(int y, const Hint* h, int n) {   // struct Hint defined at the top of the file
  oled.setFont(u8g2_font_5x7_tr);
  int x = 0;
  for (int k = 0; k < n; k++) {
    char t[24];
    if (h[k].btn >= 0) snprintf(t, sizeof(t), "%s %s", JOY_BTN_NAME[h[k].btn], h[k].act);
    else               snprintf(t, sizeof(t), "%s", h[k].act);
    int w = oled.getStrWidth(t);
    bool hot = (h[k].btn >= 0) && (h[k].btn == fbBtn) && (millis() - fbMs < 400);
    if (hot) { oled.drawBox(x, y - 7, w + 3, 9); oled.setDrawColor(0); }
    oled.drawStr(x + 1, y, t);
    if (hot) oled.setDrawColor(1);
    x += w + 8;
  }
}

// MENU pages: Config + Position + Calibration + Connection (+ Servo Bus and the
// per-servo editor when the LX-16A bus is up).
int menuPageCount() { return (bknd == 1) ? 6 : 4; }

// -------- one info page (0 Conn, 1 Position, 2 Calibration, 3 Servo Bus) --------
// Content only; the dispatcher owns clearBuffer()/dots/sendBuffer().
void drawInfoPage(uint8_t page) {
  char buf[32];
  bool hasOwnBar = false;      // Servo Bus draws its own scan/rescan bar
  if (page == 0) {                            // -------- Connection --------
    oled.setFont(u8g2_font_7x13B_tr);
    oled.drawStr(0, 12, "Connection");
    oled.drawHLine(0, 16, 128);
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 34, apMode ? "Mode: AP (hotspot)" : "Mode: Station");
    oled.drawStr(0, 52, "SSID:");
    oled.drawStr(38, 52, apMode ? AP_SSID : WiFi.SSID().c_str());
    oled.drawStr(0, 70, "IP:");
    oled.drawStr(38, 70, ip.toString().c_str());
    if (apMode) {
      oled.drawStr(0, 88, "Pass:");
      oled.drawStr(38, 88, AP_PASS);
    }
  } else if (page == 1) {                     // -------- Position --------
    // When the LX rig is the driven target, the numbers are the servos' OWN reported
    // angles (telemetry cache, user request) - "N/a" for a servo that never answers.
    // PWM has no position feedback, so that mode shows the commanded angle as before.
    bool live = (bknd == 1 && ctrlTarget != 0);
    int  pv   = live ? telPanActual  : panPos;
    int  tv   = live ? telTiltActual : tiltPos;
    oled.setFont(u8g2_font_7x13B_tr);
    oled.drawStr(0, 12, "Position");
    oled.setFont(u8g2_font_5x7_tr);
    if (homing)    oled.drawStr(92, 11, "HOMING");   // glide flag wins the corner
    else if (live) oled.drawStr(88, 11, "reported");
    oled.drawHLine(0, 16, 128);
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 36, "Pan");
    if (pv >= 0) {
      oled.setFont(u8g2_font_logisoso20_tn);        // numerals-only font
      snprintf(buf, sizeof(buf), "%d", pv);
      oled.drawStr(52, 44, buf);
    } else {
      oled.setFont(u8g2_font_7x13B_tr);             // "N/a" needs a text font
      oled.drawStr(52, 40, "N/a");
    }
    oledBar(0, 50, 128, 8, pv, panMin, panMax, homePan);      // -1 -> empty bar
    oled.setFont(u8g2_font_6x12_tr);
    oled.drawStr(0, 78, "Tilt");
    if (tv >= 0) {
      oled.setFont(u8g2_font_logisoso20_tn);
      snprintf(buf, sizeof(buf), "%d", tv);
      oled.drawStr(52, 86, buf);
    } else {
      oled.setFont(u8g2_font_7x13B_tr);
      oled.drawStr(52, 82, "N/a");
    }
    oledBar(0, 92, 128, 8, tv, tiltMin, tiltMax, homeTilt);
  } else if (page == 2) {                     // -------- Calibration --------
    oled.setFont(u8g2_font_7x13B_tr);
    oled.drawStr(0, 12, "Calibration");
    oled.drawHLine(0, 16, 128);
    oled.setFont(u8g2_font_6x12_tr);
    snprintf(buf, sizeof(buf), "Step %d  Hspd %d", stepSize, homeSpeed);
    oled.drawStr(0, 32, buf);
    snprintf(buf, sizeof(buf), "Pan lim  %d-%d", panMin, panMax);
    oled.drawStr(0, 48, buf);
    snprintf(buf, sizeof(buf), "Tilt lim %d-%d", tiltMin, tiltMax);
    oled.drawStr(0, 64, buf);
    snprintf(buf, sizeof(buf), "Home %d / %d", homePan, homeTilt);
    oled.drawStr(0, 80, buf);
    uint32_t s = millis() / 1000;
    snprintf(buf, sizeof(buf), "Up %02lu:%02lu:%02lu",
             (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60),
             (unsigned long)(s % 60));
    oled.drawStr(0, 96, buf);
  } else {                                    // -------- Servo Bus (LX-16A only) --------
    oled.setFont(u8g2_font_7x13B_tr);
    oled.drawStr(0, 12, "Servo Bus");
    oled.drawHLine(0, 16, 128);
    oled.setFont(u8g2_font_6x12_tr);
    snprintf(buf, sizeof(buf), "%s  id %d/%d", backend->name(), panid, tiltid);
    oled.drawStr(0, 32, buf);
    if (telPanVin < 0)        snprintf(buf, sizeof(buf), "Pan  --");
    else if (telPanTemp >= 0) snprintf(buf, sizeof(buf), "Pan  %d.%01dV %dC",
                                       telPanVin / 1000, (telPanVin % 1000) / 100, telPanTemp);
    else                      snprintf(buf, sizeof(buf), "Pan  %d.%01dV --C",
                                       telPanVin / 1000, (telPanVin % 1000) / 100);
    oled.drawStr(0, 50, buf);
    if (telTiltVin < 0)        snprintf(buf, sizeof(buf), "Tilt --");
    else if (telTiltTemp >= 0) snprintf(buf, sizeof(buf), "Tilt %d.%01dV %dC",
                                        telTiltVin / 1000, (telTiltVin % 1000) / 100, telTiltTemp);
    else                       snprintf(buf, sizeof(buf), "Tilt %d.%01dV --C",
                                        telTiltVin / 1000, (telTiltVin % 1000) / 100);
    oled.drawStr(0, 68, buf);
    {
      char pa[8], ta[8];
      if (telPanActual  >= 0) snprintf(pa, sizeof(pa), "%d", telPanActual);  else strcpy(pa, "--");
      if (telTiltActual >= 0) snprintf(ta, sizeof(ta), "%d", telTiltActual); else strcpy(ta, "--");
      snprintf(buf, sizeof(buf), "Act  %s / %s", pa, ta);
    }
    oled.drawStr(0, 86, buf);
    if (servoFault > 0)       snprintf(buf, sizeof(buf), "FAULT 0x%02X", servoFault);
    else if (servoFault == 0) snprintf(buf, sizeof(buf), "Status OK");
    else                      snprintf(buf, sizeof(buf), "Status --");   // -1 = no data, not "OK"
    oled.drawStr(0, 104, buf);
    hasOwnBar = true;
    // Bus-scan result + rescan hint (Confirm on this page rescans; hint flashes on press).
    if (oledScanN < 0) {
      Hint h[2] = { { mapEnter, "scan bus" }, { mapTgl, "fly" } };
      hintBar(118, h, 2);
    } else {
      static char busLine[20];
      int off = snprintf(busLine, sizeof(busLine), "Bus:");
      for (int k = 0; k < oledScanN && off < (int)sizeof(busLine) - 4; k++)
        off += snprintf(busLine + off, sizeof(busLine) - off, " %d", oledScan[k].id);
      if (oledScanN == 0) snprintf(busLine + off, sizeof(busLine) - off, " none");
      Hint h[2] = { { -1, busLine }, { mapEnter, "rescan" } };
      hintBar(118, h, 2);
    }
  }
  if (!hasOwnBar) {            // generic bar for Connection / Position / Calibration
    Hint h[2] = { { -1, "L/R page" }, { mapTgl, "fly" } };
    hintBar(114, h, 2);
  }
}

// Page-indicator dots for the MENU carousel; the active page is filled.
void drawMenuDots() {
  int n = menuPageCount();
  int x0 = 64 - (n - 1) * 6;
  for (int i = 0; i < n; i++) {
    int cx = x0 + i * 12;
    if (i == menuPage) oled.drawDisc(cx, 124, 2);
    else               oled.drawCircle(cx, 124, 2);
  }
}

// -------- MENU > Config: editable Speed / Joy mode / Step --------
void drawConfigPage() {
  char buf[24];
  oled.setFont(u8g2_font_7x13B_tr);
  oled.drawStr(0, 12, "Config");
  oled.drawHLine(0, 16, 128);
  oled.setFont(u8g2_font_6x12_tr);
  const char* labels[4] = { "Speed", "Joy mode", "Step", "Target" };
  for (int i = 0; i < 4; i++) {
    int y = 34 + i * 18;
    if (i == menuSel) { oled.drawBox(0, y - 12, 128, 16); oled.setDrawColor(0); }   // highlight row
    oled.drawStr(4, y, labels[i]);
    if      (i == 0) snprintf(buf, sizeof(buf), "%d", homeSpeed);
    else if (i == 1) snprintf(buf, sizeof(buf), "%s", joyMode ? "ABSOLUTE" : "RATE");
    else if (i == 2) snprintf(buf, sizeof(buf), "%d", stepSize);
    else             snprintf(buf, sizeof(buf), "%s",
                              ctrlTarget == 0 ? "PWM" : ctrlTarget == 1 ? "SERIAL" : "BOTH");
    bool hide = editing && i == menuSel && (millis() / 350) % 2;                    // blink while editing
    if (!hide) oled.drawStr(124 - oled.getStrWidth(buf), y, buf);
    if (i == menuSel) oled.setDrawColor(1);
  }
  if (editing) {
    Hint h[3] = { { -1, "U/D change" }, { mapEnter, "ok" }, { mapBack, "back" } };
    hintBar(110, h, 3);
  } else {
    Hint h[3] = { { -1, "U/D pick" }, { mapEnter, "edit" }, { mapTgl, "fly" } };
    hintBar(110, h, 3);
  }
}

// -------- DRIVE: live pan/tilt HUD while the stick flies the rig --------
void drawDriveHUD() {
  char buf[16];
  oled.setFont(u8g2_font_7x13B_tr);
  oled.drawStr(0, 12, "DRIVE");
  oled.setFont(u8g2_font_5x7_tr);
  oled.drawStr(84, 11, joyMode ? "ABS" : "RATE");
  oled.drawHLine(0, 16, 128);
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 36, "Pan");
  oled.setFont(u8g2_font_logisoso20_tn);
  snprintf(buf, sizeof(buf), "%d", panPos);
  oled.drawStr(52, 44, buf);
  oledBar(0, 50, 128, 8, panPos, panMin, panMax, homePan);
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 78, "Tilt");
  oled.setFont(u8g2_font_logisoso20_tn);
  snprintf(buf, sizeof(buf), "%d", tiltPos);
  oled.drawStr(52, 86, buf);
  oledBar(0, 92, 128, 8, tiltPos, tiltMin, tiltMax, homeTilt);
  {
    Hint h[2] = { { mapTgl, "menu" }, { mapHome, "home" } };
    hintBar(118, h, 2);
  }
}

// -------- MENU > Servo: one LX-16A servo's detail, with editable limits + torque (D-F) --------

// Refresh the cached per-servo config. Throttled (the bus read is ~10 transactions) and
// only called while the page is on screen and the rig is idle - a failed read leaves the
// -1 sentinels, which the page renders as "--" rather than pretending.
void svRefresh(bool force) {
  if (bknd != 1) return;
  if (!lxBusUp) { svCfgValid = false; return; }   // belt-and-braces: never touch a down bus
  uint32_t now = millis();
  if (!force && svCfgValid && now - svCfgAt < 1500) return;
  svCfg = lxCfgRead(svSel == 0 ? panid : tiltid);
  svCfgValid = true;
  svCfgAt = now;
}

// Apply one edit step (d = +1/-1) to the selected item, then re-read so the page shows
// what the servo ACTUALLY accepted (writes are range-checked inside lxCfgWrite).
// Item 1 (Bus ID) RE-LINKS the axis to a different bus ID (same as the web Link
// dropdowns) - it does NOT reprogram the servo's own ID (web-only /servoid flow, which
// has confirm+verify; a U/D stepper firing an EEPROM reprogram per step would be unsafe).
void svEdit(int item, int d) {
  if (item == 0) {                                   // pick which linked servo to inspect
    svSel = svSel ? 0 : 1;
    svCfgValid = false;
    svRefresh(true);
    return;
  }
  if (item == 1) {                                   // Bus ID: axis -> servo link (editable even
    int cur = (svSel == 0) ? panid : tiltid;         // when the current ID doesn't answer - that
    int nv  = constrain(cur + d, 0, 253);            // is exactly when you need to re-link)
    if (svSel == 0) { setPanId(nv);  backend->bind(PAN,  nv); }
    else            { setTiltId(nv); backend->bind(TILT, nv); }
    svCfgValid = false;                              // shown config belongs to the new ID now
    svRefresh(true);
    return;
  }
  if (!svCfgValid) return;                           // never write blind
  int id = (svSel == 0) ? panid : tiltid;
  switch (item) {
    case 2: if (svCfg.alimMin >= 0 && svCfg.alimMax >= 0)
              lxCfgWrite(id, "alim", constrain(svCfg.alimMin + d * 10, 0, svCfg.alimMax - 1), svCfg.alimMax);
            break;
    case 3: if (svCfg.alimMin >= 0 && svCfg.alimMax >= 0)
              lxCfgWrite(id, "alim", svCfg.alimMin, constrain(svCfg.alimMax + d * 10, svCfg.alimMin + 1, 1000));
            break;
    case 4: if (svCfg.trim > -128)                   // trim (RAM), -125..125, 1 unit = 0.24 deg;
              lxCfgWrite(id, "trim", constrain(svCfg.trim + d, -125, 125));   // -128 = the trim
            break;                                   // READ failed (review: posDeg was a bad proxy)
    case 5: lxCfgWrite(id, "trimsave", 0);           // ACTION: persist trim to servo EEPROM
            break;
    case 6: if (svCfg.vlimMin >= 0 && svCfg.vlimMax >= 0)
              lxCfgWrite(id, "vlim", constrain(svCfg.vlimMin + d * 100, 4500, svCfg.vlimMax - 100), svCfg.vlimMax);
            break;
    case 7: if (svCfg.vlimMin >= 0 && svCfg.vlimMax >= 0)
              lxCfgWrite(id, "vlim", svCfg.vlimMin, constrain(svCfg.vlimMax + d * 100, svCfg.vlimMin + 100, 12000));
            break;
    case 8: if (svCfg.tlim >= 0)
              lxCfgWrite(id, "tlim", constrain(svCfg.tlim + d, 50, 100));
            break;
    case 9: if (svCfg.torque >= 0)
              lxCfgWrite(id, "torque", svCfg.torque ? 0 : 1);
            break;
    case 10: if (svCfg.led >= 0)                     // protocol: 0 = LED ON, 1 = OFF (inverted)
              lxCfgWrite(id, "led", svCfg.led ? 0 : 1);
            break;
    case 11: if (svCfg.lederr >= 0)                  // alarm mask 0..7 (temp/volt/stall bits)
              lxCfgWrite(id, "lederr", constrain(svCfg.lederr + d, 0, 7));
            break;
    case 12: if (svCfg.mode >= 0)                    // SERVO <-> MOTOR: always enter MOTOR at
              lxCfgWrite(id, "mode", svCfg.mode ? 0 : 1, 0);   // speed 0, never spinning
            break;
    case 13: if (svCfg.mode >= 0)                    // motor speed (only acts in MOTOR mode)
              lxCfgWrite(id, "mode", svCfg.mode, constrain((svCfg.speed > -1001 ? svCfg.speed : 0) + d * 50, -1000, 1000));
            break;
  }
  svRefresh(true);
}

// OLED-triggered bus rescan (Servo Bus page, Confirm button). Blocking ~10ms per absent
// ID x 12 = ~150ms worst case - fine for an explicit manual action.
void oledRunScan() {
  if (!lxBusUp) lxBusBegin();
  oledScanN = lxScanRange(12, oledScan, 8);
}

void drawServoPage() {
  char buf[28];
  oled.setFont(u8g2_font_7x13B_tr);
  oled.drawStr(0, 12, "Servo");
  oled.setFont(u8g2_font_5x7_tr);
  snprintf(buf, sizeof(buf), "id %d", svSel == 0 ? panid : tiltid);
  oled.drawStr(100, 11, buf);
  oled.drawHLine(0, 16, 128);
  oled.setFont(u8g2_font_6x12_tr);

  // Full LX-16A setting list (parity with the SpaceMaster85 / lewansoul-lx16a tools),
  // windowed 5 rows at a time; the selection auto-scrolls the window.
  static const char* labels[SV_ITEMS] = {
    "Servo", "Bus ID", "Lim min", "Lim max", "Trim", "Trim save",
    "V min", "V max", "T max", "Torque", "LED", "Alarm", "Mode", "Speed"
  };
  int first = constrain(menuSel - 2, 0, SV_ITEMS - 5);
  for (int r = 0; r < 5; r++) {
    int i = first + r;
    int y = 32 + r * 16;
    if (i == menuSel) { oled.drawBox(0, y - 12, 128, 15); oled.setDrawColor(0); }
    oled.drawStr(4, y, labels[i]);
    bool ok = svCfgValid;
    switch (i) {
      case 0:  snprintf(buf, sizeof(buf), "%s", svSel == 0 ? "PAN" : "TILT"); break;
      case 1:  snprintf(buf, sizeof(buf), "%d", (svSel == 0) ? panid : tiltid); break;
      case 2:  if (ok && svCfg.alimMin >= 0) snprintf(buf, sizeof(buf), "%d", svCfg.alimMin); else strcpy(buf, "--"); break;
      case 3:  if (ok && svCfg.alimMax >= 0) snprintf(buf, sizeof(buf), "%d", svCfg.alimMax); else strcpy(buf, "--"); break;
      case 4:  if (ok && svCfg.trim > -128)  snprintf(buf, sizeof(buf), "%d", svCfg.trim);    else strcpy(buf, "--"); break;
      case 5:  strcpy(buf, "[OK]"); break;                                    // action row
      case 6:  if (ok && svCfg.vlimMin >= 0) snprintf(buf, sizeof(buf), "%d", svCfg.vlimMin); else strcpy(buf, "--"); break;
      case 7:  if (ok && svCfg.vlimMax >= 0) snprintf(buf, sizeof(buf), "%d", svCfg.vlimMax); else strcpy(buf, "--"); break;
      case 8:  if (ok && svCfg.tlim    >= 0) snprintf(buf, sizeof(buf), "%dC", svCfg.tlim);   else strcpy(buf, "--"); break;
      case 9:  if (ok && svCfg.torque  >= 0) strcpy(buf, svCfg.torque ? "ON" : "OFF");        else strcpy(buf, "--"); break;
      case 10: if (ok && svCfg.led     >= 0) strcpy(buf, svCfg.led ? "OFF" : "ON");           else strcpy(buf, "--"); break;  // inverted register
      case 11: if (ok && svCfg.lederr  >= 0) snprintf(buf, sizeof(buf), "%d", svCfg.lederr);  else strcpy(buf, "--"); break;
      case 12: if (ok && svCfg.mode    >= 0) strcpy(buf, svCfg.mode ? "MOTOR" : "SERVO");     else strcpy(buf, "--"); break;
      default: if (ok && svCfg.mode    >= 0) snprintf(buf, sizeof(buf), "%d", svCfg.speed);   else strcpy(buf, "--"); break;
    }
    bool hide = editing && i == menuSel && (millis() / 350) % 2;
    if (!hide) oled.drawStr(124 - oled.getStrWidth(buf), y, buf);
    if (i == menuSel) oled.setDrawColor(1);
  }
  if (first > 0)                 oled.drawStr(120, 24, "^");     // more rows above
  if (first + 5 < SV_ITEMS)      oled.drawStr(120, 100, "v");    // more rows below

  oled.setFont(u8g2_font_5x7_tr);
  if (svCfgValid && svCfg.posDeg >= 0 && svCfg.vinMv >= 0)
    snprintf(buf, sizeof(buf), "pos %d  %d.%01dV  %dC", svCfg.posDeg,
             svCfg.vinMv / 1000, (svCfg.vinMv % 1000) / 100, svCfg.tempC);
  else strcpy(buf, "no reply on the bus");
  oled.drawStr(0, 108, buf);
  if (editing) {
    Hint h[3] = { { -1, "U/D change" }, { mapEnter, "ok" }, { mapBack, "back" } };
    hintBar(118, h, 3);
  } else {
    Hint h[3] = { { -1, "U/D" }, { mapEnter, "edit" }, { mapBack, "back" } };
    hintBar(118, h, 3);
  }
}

// Render the cockpit into the full-frame buffer and push it over I2C.
void drawOLED() {
  if (!oledPresent) return;
  oled.clearBuffer();
  if (cockMode == CM_DRIVE) { drawDriveHUD(); oled.sendBuffer(); return; }
  if (menuPage == 5 && bknd == 1 && !homing) svRefresh(false);   // page-visible, idle-only bus read
  switch (menuPage) {                         // MENU carousel
    case 0:  drawConfigPage(); break;         // editable
    case 1:  drawInfoPage(1);  break;         // Position
    case 2:  drawInfoPage(2);  break;         // Calibration
    case 3:  drawInfoPage(0);  break;         // Connection
    case 4:  drawInfoPage(3);  break;         // Servo Bus (LX only)
    default: drawServoPage();  break;         // per-servo detail + edit (LX only)
  }
  drawMenuDots();
  oled.sendBuffer();
}

// ============================================================
//  Section 8 - WS2812B: mood light when idle, direction animation when moving
// ============================================================

// Per-direction colours (each move direction gets its own colour).
uint32_t animColor() {
  switch (ledAnim) {
    case LED_PAN_CW:    return leds.Color(0, 90, 255);    // pan right - blue
    case LED_PAN_CCW:   return leds.Color(255, 0, 170);   // pan left  - magenta
    case LED_TILT_UP:   return leds.Color(0, 230, 40);    // tilt up   - green
    case LED_TILT_DOWN: return leds.Color(255, 90, 0);    // tilt down - amber
    case LED_FAULT:     return leds.Color(120, 0, 0);     // latched fault - solid red
    default:            return 0;
  }
}

// A dim (~20%) version of a packed colour, for the comet trail.
uint32_t dimColor(uint32_t c) {
  return leds.Color(((c >> 16) & 0xFF) / 5, ((c >> 8) & 0xFF) / 5, (c & 0xFF) / 5);
}

// Called by nudge()/updateHoming() with the SIGNED actual position change of one
// axis (0 = clamped / no movement -> no animation). Selects the matching pattern.
void signalMove(Axis axis, int deltaPos) {
  if (deltaPos == 0) return;
  if (axis == PAN)  ledAnim = (deltaPos > 0) ? LED_PAN_CW : LED_PAN_CCW;    // +angle = clockwise
  else              ledAnim = (deltaPos > 0) ? LED_TILT_UP : LED_TILT_DOWN; // +angle = rising
  ledActiveUntil = millis() + LED_HOLD_MS;
}

void setupLeds() {
  leds.begin();
  leds.setBrightness(255);        // brightness is baked into the colours we write
  leds.clear();
  leds.show();
}

// Non-blocking. While moving: animate the direction pattern in its colour.
// While idle: a slow ~30% rainbow "mood light" across all 4 corners.
void updateLeds() {
  uint32_t now = millis();

  if (now < ledActiveUntil && ledAnim != LED_IDLE) {           // ---- MOVING ----
    if (now - ledLastFrame < LED_FRAME_MS) return;
    ledLastFrame = now;
    ledStep++;
    leds.clear();
    uint32_t c = animColor();
    if (ledAnim == LED_PAN_CW || ledAnim == LED_PAN_CCW) {
      static const int cw[4]  = { LED_TR, LED_BR, LED_BL, LED_TL };  // clockwise
      static const int ccw[4] = { LED_TR, LED_TL, LED_BL, LED_BR };  // anti-clockwise
      const int* seq = (ledAnim == LED_PAN_CW) ? cw : ccw;
      int head = ledStep % 4, tail = (head + 3) % 4;
      leds.setPixelColor(seq[head], c);
      leds.setPixelColor(seq[tail], dimColor(c));               // comet trail
    } else {
      bool topFirst = (ledAnim == LED_TILT_DOWN);               // down = top then bottom
      bool showTop  = ((ledStep % 2) == 0) ? topFirst : !topFirst;
      if (showTop) { leds.setPixelColor(LED_TR, c); leds.setPixelColor(LED_TL, c); }
      else         { leds.setPixelColor(LED_BL, c); leds.setPixelColor(LED_BR, c); }
    }
    leds.show();
    return;
  }

  // ---- FAULT: solid-red override (LX-16A). servoFault is latched by the N-failure /
  //      threshold logic in deriveFault(); it takes precedence over the idle mood
  //      light and clears automatically when telemetry recovers. (-1/0 => not shown.) ----
  if (servoFault > 0) {
    if (ledAnim != LED_FAULT) { ledAnim = LED_FAULT; oledDirty = true; }
    if (now - moodLastFrame < MOOD_FRAME_MS) return;   // reuse the mood cadence to throttle shows
    moodLastFrame = now;
    uint32_t red = animColor();                        // LED_FAULT -> red
    for (int i = 0; i < NUM_LEDS; i++) leds.setPixelColor(i, red);
    leds.show();
    return;
  }

  // ---- IDLE: slow mood rainbow (~30%) across all 4 corners ----
  if (ledAnim != LED_IDLE) { ledAnim = LED_IDLE; oledDirty = true; }  // motion ended -> refresh OLED
  if (now - moodLastFrame < MOOD_FRAME_MS) return;
  moodLastFrame = now;
  moodHue += 160;                                                // slow drift (~25 s per full cycle)
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t h = moodHue + (uint16_t)(i * (65536 / NUM_LEDS));
    leds.setPixelColor(i, leds.gamma32(leds.ColorHSV(h, 255, MOOD_VAL)));
  }
  leds.show();
}

// OLED "moving" screen: a big direction arrow + message + live angles. Shown by
// drawOLED() while a motion animation is active, replacing the cycling pages.
void drawMotionScreen() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x13B_tr);
  oled.drawStr(0, 12, "Moving");
  oled.drawHLine(0, 16, 128);

  const int cx = 64, cy = 56;
  const char* msg = "";
  if (ledAnim == LED_PAN_CW) {                    // pan right ->
    msg = "PAN RIGHT";
    oled.drawBox(cx - 22, cy - 4, 30, 8);
    oled.drawTriangle(cx + 8, cy - 14, cx + 8, cy + 14, cx + 24, cy);
  } else if (ledAnim == LED_PAN_CCW) {            // pan left <-
    msg = "PAN LEFT";
    oled.drawBox(cx - 8, cy - 4, 30, 8);
    oled.drawTriangle(cx - 8, cy - 14, cx - 8, cy + 14, cx - 24, cy);
  } else if (ledAnim == LED_TILT_UP) {            // tilt up ^
    msg = "TILT UP";
    oled.drawBox(cx - 4, cy - 8, 8, 30);
    oled.drawTriangle(cx - 14, cy - 8, cx + 14, cy - 8, cx, cy - 24);
  } else {                                        // tilt down v
    msg = "TILT DOWN";
    oled.drawBox(cx - 4, cy - 22, 8, 30);
    oled.drawTriangle(cx - 14, cy + 8, cx + 14, cy + 8, cx, cy + 24);
  }

  oled.setFont(u8g2_font_7x13B_tr);
  int w = oled.getStrWidth(msg);
  oled.drawStr((128 - w) / 2, 96, msg);

  oled.setFont(u8g2_font_6x12_tr);
  char buf[24];
  snprintf(buf, sizeof(buf), "Pan %d   Tilt %d", panPos, tiltPos);
  int bw = oled.getStrWidth(buf);
  oled.drawStr((128 - bw) / 2, 118, buf);

  oled.sendBuffer();
}

// ============================================================
//  Section 9 - I2C joystick (nulllab mini @ 0x5A): the cockpit's physical input
// ============================================================
uint32_t joyLast = 0, joyPanJog = 0, joyTiltJog = 0;

void setupJoystick() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);       // shared bus; ensures 21/22 even if the OLED is absent
  Serial.print(F("I2C devices:"));              // scan - logs everything that ACKs
  for (uint8_t a = 8; a < 0x78; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a); }
  Serial.println();
  Wire.beginTransmission(0x5A);                 // nulllab mini-joystick module (raw I2C registers)
  if (Wire.endTransmission() == 0) { joyMini = true; Serial.println(F("Joystick: nulllab mini @ 0x5A")); }
  else Serial.println(F("Joystick: none (no 0x5A on the bus)"));
}

// Read one register from the nulllab mini-joystick (0x5A): write reg with a full STOP,
// then read 1 byte (matches the JoystickHandle library's handshake).
// Returns -1 on ANY I2C failure. 0xFF is a legitimate full-deflection axis value, so
// failure MUST be signalled out-of-band: the audit confirmed that decoding a failed read
// as 0xFF turned a transient bus glitch (servo EMI, loose wire) into a hard-right stick
// -> uncommanded max-rate runaway, which could also walk the MENU on its own.
int miniRead(uint8_t reg) {
  Wire.beginTransmission(0x5A);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return -1;              // full stop (NOT repeated-start)
  if (Wire.requestFrom((uint8_t)0x5A, (uint8_t)1) != 1) return -1;
  return Wire.available() ? Wire.read() : -1;
}
int joyMiniFails = 0;   // consecutive failed ticks; stick declared offline at 100 (reboot re-detects)

// Discrete stick step with key-repeat for MENU navigation. Returns -1/0/+1; re-arms
// when the stick returns to the deadzone, then repeats every 300ms while held.
int stickStep(int defl, int dead, int8_t &armed, uint32_t &tmr) {
  if (abs(defl) <= dead) { armed = 0; return 0; }
  int dir = defl > 0 ? 1 : -1;
  uint32_t now = millis();
  if (armed != dir)     { armed = dir; tmr = now; return dir; }
  if (now - tmr >= 300) { tmr = now; return dir; }
  return 0;
}

// Non-blocking cockpit input. C toggles DRIVE/MENU. DRIVE: stick flies the active
// target (RATE jog or ABSOLUTE glide), A homes, 5s idle -> MENU. MENU: stick U/D =
// item, L/R = page, OK = edit/confirm, B = back.
void readJoystick() {
  if (testBusy()) return;                   // range test owns the rig
  if (!joyEnabled || !joyMini) return;
  uint32_t now = millis();
  if (now - joyLast < JOY_MS) return;
  joyLast = now;

  // nulllab: X=reg 0x10, Y=reg 0x11, 0-255 centre 128.
  // Double-read consistency check: a half-seated module can return SUCCESSFUL reads
  // carrying garbage (railed values), which the -1 failure guard cannot see - measured
  // on this bench driving the rig to its soft limit unattended. A real stick cannot
  // teleport between two reads ~1ms apart, so disagreement >30 counts = noise, not input.
  int xd, yd;
  const int dead = JOY_DEAD, full = JOY_FULL;
  {
    int rx  = miniRead(0x10), ry  = miniRead(0x11);
    int rx2 = miniRead(0x10), ry2 = miniRead(0x11);
    if (rx < 0 || ry < 0 || rx2 < 0 || ry2 < 0 ||
        abs(rx - rx2) > 30 || abs(ry - ry2) > 30) { // glitch OR inconsistent: inert tick
      if (++joyMiniFails == 100) { joyMini = false; Serial.println(F("Joystick offline (I2C failures)")); }
      return;
    }
    joyMiniFails = 0;
    xd = rx2 - 128;                                 // second read: past any bus settling
    yd = ry2 - 128;
  }

  // Edge-detected button clicks this tick (event 3 = SINGLE_CLICK on the mini).
  // The stick-click (OK) is stiff to press, so it is unused: C = confirm/edit,
  // D = toggle DRIVE/MENU, A = home, B = back.
  bool bTgl = false, bEnter = false, bB = false, bA = false;
  {                                                 // read all 5 buttons, dispatch through the remap
    static uint8_t pv[5] = { 8, 8, 8, 8, 8 };
    bool click[5];
    for (int i = 0; i < 5; i++) {
      int v = miniRead(JOY_BTN_REG[i]);
      if (v > 8) v = -1;                            // valid event codes are 0..8; else garbage
      if (v == 3) {                                 // click candidate: confirm plausibility with a
        int v2 = miniRead(JOY_BTN_REG[i]);          // second read - a real click reads 3 again (or
        if (v2 < 0 || v2 > 8) v = -1;               // 8 if the event cleared); garbage reads random
      }
      click[i] = (v == 3 && pv[i] != 3);            // failed read (-1): no click, keep prev state
      if (v >= 0) pv[i] = (uint8_t)v;
    }
    bA = click[mapHome]; bB = click[mapBack]; bEnter = click[mapEnter]; bTgl = click[mapTgl];
  }

  // Button-press feedback: remember which mapped button just clicked so the hint
  // bar can flash it inverted (visible acknowledgment on every page).
  if (bA)     { fbBtn = mapHome;  fbMs = now; oledDirty = true; }
  if (bB)     { fbBtn = mapBack;  fbMs = now; oledDirty = true; }
  if (bEnter) { fbBtn = mapEnter; fbMs = now; oledDirty = true; }
  if (bTgl)   { fbBtn = mapTgl;   fbMs = now; oledDirty = true; }

  if (bTgl) {                                       // D: toggle DRIVE <-> MENU anytime
    cockMode = (cockMode == CM_DRIVE) ? CM_MENU : CM_DRIVE;
    editing = false; driveIdleSince = now; oledDirty = true;
    return;
  }

  if (cockMode == CM_DRIVE) {
    if (bA) startHome();
    bool active = false;
    if (joyMode == 0) {                             // RATE: proportional-rate jog
      if (abs(xd) > dead) {
        uint16_t iv = map(min(abs(xd), full), dead, full, JOY_SLOW_MS, JOY_FAST_MS);
        if (now - joyPanJog  >= iv) { nudge(PAN,  xd > 0 ? +1 : -1); joyPanJog  = now; }
        active = true;
      }
      if (abs(yd) > dead) {
        uint16_t iv = map(min(abs(yd), full), dead, full, JOY_SLOW_MS, JOY_FAST_MS);
        if (now - joyTiltJog >= iv) { nudge(TILT, yd < 0 ? +1 : -1); joyTiltJog = now; }  // stick up = tilt up
        active = true;
      }
    } else {                                        // ABSOLUTE: stick maps to position, glide (never snap)
      int gstep = max(1, homeSpeed * (int)JOY_MS / 1000);
      int pt = map(constrain(xd, -full, full), -full, full, panMin, panMax);
      int tt = map(constrain(yd, -full, full),  full, -full, tiltMin, tiltMax);   // stick up -> higher tilt
      if (panPos != pt)  { int b0 = panPos;  panPos  = (panPos  < pt) ? min(pt, panPos  + gstep) : max(pt, panPos  - gstep); driveAngle(PAN,  panPos);  signalMove(PAN,  panPos  - b0); }
      if (tiltPos != tt) { int b0 = tiltPos; tiltPos = (tiltPos < tt) ? min(tt, tiltPos + gstep) : max(tt, tiltPos - gstep); driveAngle(TILT, tiltPos); signalMove(TILT, tiltPos - b0); }
      if (abs(xd) > dead || abs(yd) > dead) active = true;
    }
    if (active) driveIdleSince = now;
    else if (now - driveIdleSince > 5000) { cockMode = CM_MENU; oledDirty = true; }   // D-G idle -> MENU
    oledDirty = true;
    return;
  }

  // ---- MENU ----
  static int8_t armX = 0, armY = 0;
  static uint32_t tmrX = 0, tmrY = 0;
  int sx = stickStep(xd, dead, armX, tmrX);
  int sy = stickStep(yd, dead, armY, tmrY);
  int items = 0;                                    // editable rows on this page
  if      (menuPage == 0) items = 4;                // Config: speed / joy mode / step / target
  else if (menuPage == 5) items = SV_ITEMS;         // Servo: full per-servo setting list

  if (bB) { editing = false; oledDirty = true; return; }   // back / cancel edit

  // Servo Bus page: Confirm = rescan the bus (info page, no edit items).
  if (bEnter && menuPage == 4 && bknd == 1) { oledRunScan(); oledDirty = true; return; }

  if (editing) {
    if (sy != 0) {                                  // stick up (sy<0) increases the value
      int d = -sy;
      if (menuPage == 0) {
        if      (menuSel == 0) setHomeSpeed(homeSpeed + d * 5);
        else if (menuSel == 1) setJoyMode(d > 0 ? 1 : 0);    // up = ABSOLUTE, down = RATE
        else if (menuSel == 2) setStep(stepSize + d);
        else                   setCtrlTarget((ctrlTarget + (d > 0 ? 1 : 2)) % 3);   // cycle the
                               // driven servo set from the stick (clamped to PWM if no LX bus)
      } else if (menuPage == 5) {
        svEdit(menuSel, d);
      }
      oledDirty = true;
    }
    if (bEnter) { editing = false; oledDirty = true; }         // confirm (values persist live)
  } else {
    if (sy != 0 && items > 0) { menuSel = constrain(menuSel + sy, 0, items - 1); oledDirty = true; }
    if (sx != 0) { menuPage = (menuPage + sx + menuPageCount()) % menuPageCount(); menuSel = 0; oledDirty = true; }
    if (bEnter && items > 0) {
      if (menuPage == 5 && svIsAction(menuSel)) { svEdit(menuSel, 0); oledDirty = true; }  // action row: execute
      else                                      { editing = true;     oledDirty = true; }
    }
  }
}

// ============================================================
//  Arduino entry points
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("ESP32 Pan/Tilt starting..."));

  // Status LED
  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    setLed(false);
  }

  // WS2812B corner LEDs (dark until something moves).
  setupLeds();

  // OLED (shows a splash now; the 3 pages start once WiFi mode/IP are known).
  setupOLED();

  // Restore soft limits / home / speed / step from NVS (validated) BEFORE the
  // servos move, so the rig powers up at its calibrated home.
  loadSettings();

  // Servo backend: pick from the loaded bknd (default 0 = PWM/SG90). begin() OWNS the
  // whole bring-up - timers/period/attach + home write for PWM, or bus open/prime/
  // widen-limits/protection/gentle-home/torque for LX-16A - including the
  // panPos/tiltPos = homePan/homeTilt assignment, so the rig powers up at home.
  oledPageCount = (bknd == 1) ? 4 : 3;      // LX-16A adds the "Servo Bus" OLED page
  if (bknd != 1) ctrlTarget = 0;            // can't route to a bus that isn't up
  // Each backend parks at ITS OWN target's home (audit: booting with a non-zero active
  // target used to home the PWM rig to the LX target's calibration, outside its own
  // window). The live home globals are pointed at each slot around its begin(); the
  // loadSlot() below then makes the ACTIVE target's position + config live again.
  homePan = ctrlSlot[0].homePan;  homeTilt = ctrlSlot[0].homeTilt;
  pwmB.begin();                             // PWM always up (SG90 on 25/26)
  if (bknd == 1) {
    homePan = ctrlSlot[1].homePan;  homeTilt = ctrlSlot[1].homeTilt;
    lxB.begin();                            // LX bus up too, so both can be driven and tested
  }
  backend = (bknd == 1) ? (ServoBackend*)&lxB : (ServoBackend*)&pwmB;   // active = telemetry source
  Serial.printf("Servo backends: PWM up%s; target=%d (pan attached=%d tilt=%d)\n",
                bknd == 1 ? " + LX16A up" : "", ctrlTarget,
                panServo.attached(), tiltServo.attached());
  // Bookkeeping: every target starts parked at its OWN home (already validated inside
  // its own window by loadSettings). Assign the position/glide fields MEMBER-WISE: a
  // short brace-list here would value-initialize (zero) the config members. Note the
  // "Both" slot (2) tracks its own home; the physical rigs sit at slots 0/1's homes
  // until the first drive under target 2 syncs them.
  for (int i = 0; i < 3; i++) {
    ctrlSlot[i].pan        = ctrlSlot[i].homePan;
    ctrlSlot[i].tilt       = ctrlSlot[i].homeTilt;
    ctrlSlot[i].homing     = false;
    ctrlSlot[i].homeLastMs = 0;
  }
  loadSlot(ctrlTarget);                     // live globals = the active target's state
  setupJoystick();                          // I2C joystick (auto-detect; no-op if absent)

  // WiFi: Station first, Access-Point fallback.
  connectWiFi();

  // First real OLED frame now that WiFi mode/IP are known.
  oledPage = 0;
  oledDirty = true;
  drawOLED();
  oledLastPage = millis();
  oledLastDraw = millis();

  // HTTP routes (all GET).
  server.on("/",           HTTP_GET, handleRoot);
  server.on("/action",     HTTP_GET, handleAction);
  server.on("/status",     HTTP_GET, handleStatus);
  server.on("/scan",       HTTP_GET, handleScan);
  server.on("/setwifi",    HTTP_GET, handleSetWifi);
  server.on("/forgetwifi", HTTP_GET, handleForgetWifi);
  server.on("/lxprobe",    HTTP_GET, handleLxProbe);      // read-only bus probe (safe in PWM mode)
  server.on("/lxpintest",  HTTP_GET, handleLxPinTest);    // electrical check: is anything on the pin?
  server.on("/lxwiggle",   HTTP_GET, handleLxWiggle);     // broadcast move: ID-agnostic, write-only
  server.on("/lxtx",       HTTP_GET, handleLxTx);         // is the ESP32 transmitting at all? (D1)
  server.on("/lxfix",      HTTP_GET, handleLxFix);        // force half-duplex re-arm + sweep
  server.on("/lxcal",      HTTP_GET, handleLxCal);        // auto-trim: learned overshoots + records
  server.on("/servotest",  HTTP_GET, handleServoTest);    // slow full-range sweep + collision watch
  server.on("/servoscan",  HTTP_GET, handleServoScan);    // LX-16A bus discovery
  server.on("/servoid",    HTTP_GET, handleServoId);      // LX-16A safe ID programming
  server.on("/servocfg",   HTTP_GET, handleServoCfg);     // LX-16A per-servo config read/write
  server.on("/setbackend", HTTP_GET, handleSetBackend);   // switch PWM <-> LX-16A (persist + reboot)
  server.on("/setlxpin",   HTTP_GET, handleSetLxPin);     // change the LX bus GPIO (persist + reboot)
  server.on("/setpwmpins", HTTP_GET, handleSetPwmPins);   // change the PWM servo GPIOs (persist + reboot)
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("HTTP server started on port 80"));
}

void loop() {
  server.handleClient();

  // Advance the smooth home glide (non-blocking; no-op unless homing).
  updateHoming();

  // Physical I2C joystick input (rate-jog + buttons); no-op if none present.
  readJoystick();

  // Corner LEDs: animate while moving, latched red on fault, mood light when idle.
  updateLeds();

  // LX-16A telemetry: throttled, idle-only best-effort bus reads (no-op in PWM mode).
  updateTelemetry();

  // Stiction auto-trim: settle-check + staircase correction + learning (no-op unless LX driven).
  updateLxTrim();

  // Servo range test: slow full-travel sweep + collision watch (no-op when idle).
  updateServoTest();

  // OLED cockpit: no auto page-advance (the joystick navigates). Refresh at OLED_DRAW_MS
  // so the DRIVE HUD and MENU values track state; the full-frame push never hogs loop().
  if (oledPresent) {
    if (millis() - oledLastPage >= OLED_DRAW_MS) { oledLastPage = millis(); oledDirty = true; }
    if (oledDirty && millis() - oledLastDraw >= OLED_DRAW_MS) {
      oledDirty = false;
      oledLastDraw = millis();
      drawOLED();
    }
  }

  // Non-blocking slow-blink of the status LED while in AP mode.
  if (apMode && STATUS_LED_PIN >= 0) {
    static uint32_t last = 0;
    static bool     on   = false;
    if (millis() - last >= LED_SLOW_MS) {
      last = millis();
      on = !on;
      digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
    }
  }
}
