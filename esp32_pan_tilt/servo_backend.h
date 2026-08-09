// ============================================================
//  servo_backend.h  -  Servo-backend abstraction (PWM  +  LX-16A serial bus)
//
//  Implements the ServoBackend seam from serial-servo.md section 5. The whole
//  firmware already speaks a backend-agnostic 0..180 integer-degree model
//  (panPos/tiltPos, soft limits, home, glide, invert, OLED, LEDs). This header
//  wraps the ONLY PWM-coupled sites behind a tiny interface so an LX-16A serial
//  backend can slot in alongside the existing SG90/PWM one, chosen at boot.
//
//  DESIGN INVARIANTS (do not violate):
//   - PWM (bknd=0) is the DEFAULT and stays 100% behavior-preserving. The LX-16A
//     path is purely additive; both backends are compiled in, one is active/boot.
//   - Reads on the one-pin half-duplex bus are BEST-EFFORT (echo loopback): every
//     read is gated on isCommandOk() and the bus runs with retry=0 (5.5).
//   - NEVER call LX16AServo::calibrate() - it can while(1)-hang (quirk 3.1). We
//     use setLimitsTicks() directly instead.
//   - The lx16a-servo LIBRARY is used for its command constants ONLY: its bus objects
//     and write path are dead on core 3.x (pinMode detaches the pad - lessonlearn 20/21).
//     All bus traffic goes through the half-duplex engine below (lxRawSend/lxRawRead).
//
//  INCLUDE EXACTLY ONCE, from esp32_pan_tilt.ino, next to #include "web_page.h".
//  It is #included ABOVE the .ino's own global definitions, so the .ino globals
//  it wraps are forward-declared `extern` below (same translation unit resolves
//  them). The .ino must delete its duplicate `enum Axis` once this is included.
// ============================================================
#pragma once

#include <Arduino.h>
#include <string.h>        // memset / strcmp (config read/write helpers)
#include <ESP32Servo.h>    // Servo, ESP32PWM  (PWM backend)
#include <lx16a-servo.h>   // LX16A_SERVO_* command constants ONLY (no library objects - see below)
#include <esp_rom_gpio.h>       // esp_rom_gpio_connect_out_signal / pad_select_gpio (half-duplex)
#include <soc/gpio_sig_map.h>   // U2TXD_OUT_IDX
#include <driver/gpio.h>        // gpio_pullup_en / gpio_set_direction (never pinMode on the bus pad)

// ------------------------------------------------------------
//  Axis - moved here from the .ino so the interface can reference it.
//  (The .ino's duplicate `enum Axis { PAN, TILT };` must be deleted.)
//  Values are load-bearing as array indices: PAN=0, TILT=1.
// ------------------------------------------------------------
enum Axis { PAN, TILT };

// ------------------------------------------------------------
//  LX-16A compile-time configuration (v1 keeps these as consts; the plan's
//  optional `lxpin` / `lxmove` NVS keys are deferred - section 8).
// ------------------------------------------------------------
static const int LX_PIN_DEFAULT = 32;   // one-wire half-duplex bus GPIO, FIRST-BOOT default only.
                                        // The live value is the NVS-backed `lxpin` (web-settable);
                                        // the half-duplex engine routes UART RX + TX to it via the
                                        // GPIO matrix, so it need not be a Serial2 default pin.
static const int LX_MS_FLOOR   = 40;    // min move time, ms  (smooth 1-deg glide steps) - D6
static const int LX_MS_CEIL    = 220;   // max move time, ms  (big jumps / reclamp yanks)  - D6
static const int LX_MS_K       = 6;     // move-time slope, ms per commanded degree        - D6
static const int LX_HOME_MS    = 800;   // gentle power-on glide-to-home duration, ms (5.6)
static const int LX_VIN_MIN_MV = 6000;  // conservative under-voltage cutoff, mV (5.6 / 10)
static const int LX_VIN_MAX_MV = 8400;  // conservative over-voltage cutoff, mV  (never >8.4V)
static const int LX_TEMP_CAP_C = 70;    // conservative over-temperature cutoff, deg C

// ------------------------------------------------------------
//  Globals owned & DEFINED by esp32_pan_tilt.ino. Because this header expands
//  above those definitions, they are forward-declared extern here; the single
//  translation unit resolves them to the .ino's definitions further down.
//
//  NOTE on INVERT_PAN / INVERT_TILT: intentionally NOT wrapped. Per section 5.3
//  invert stays at the angle layer (it negates the delta inside nudge(), above
//  the write layer) and is backend-agnostic - neither backend touches it.
// ------------------------------------------------------------
extern Servo     panServo, tiltServo;         // ESP32Servo objects (PWM backend targets)
extern int       panPos,   tiltPos;           // live logical position, 0..180 deg
extern int       homePan,  homeTilt;          // calibrated home target, 0..180 deg
extern int       panid,    tiltid;            // LX-16A bus IDs (NVS-owned identity; .ino owns)
extern int       lxpin;                       // live LX bus GPIO (NVS "lxpin"; .ino owns)
extern int       pwmPanPin, pwmTiltPin;       // PWM GPIOs (NVS "ppin"/"tpin"; .ino owns)
extern const int SERVO_MIN_US, SERVO_MAX_US;  // PWM pulse range, us (500 / 2500)

// Can `p` serve as the one-wire LX bus pin on this board? The bus must both drive and
// read the line, and a bad choice can hang the board or fight another peripheral - so
// this is validated in the firmware, never trusted from the client. Returns nullptr if
// the pin is usable, else a short reason suitable for the web UI.
static inline const char* lxPinReject(int p) {
  if (p < 0 || p > 39)                        return "out of range (0-39)";
  if (p >= 34)                                return "input-only (34-39)";
  if (p >= 6 && p <= 11)                      return "SPI flash pin";
  if (p == 1 || p == 3)                       return "UART0 console";
  if (p == 0 || p == 2 || p == 12 || p == 15) return "boot strapping pin";
  if (p == pwmPanPin || p == pwmTiltPin)      return "pan/tilt servo pin";
  if (p == 21 || p == 22)                     return "I2C bus (OLED/joystick)";
  if (p == 16)                                return "WS2812B data pin";
  return nullptr;                             // usable: 4,5,13,14,17,18,19,23,27,32,33
}

// Can `p` serve as a PWM servo pin? Same hardware constraints as the LX pin, plus it
// must not collide with the LX bus pin or the OTHER pwm pin (pass that as `other`).
// Firmware-side authority: the web dropdowns merely pre-filter for convenience.
static inline const char* pwmPinReject(int p, int other) {
  extern int lxpin;
  if (p < 0 || p > 33)                        return "out of range (0-33)";
  if (p >= 34)                                return "input-only (34-39)";
  if (p >= 6 && p <= 11)                      return "SPI flash pin";
  if (p == 1 || p == 3)                       return "UART0 console";
  if (p == 0 || p == 2 || p == 12 || p == 15) return "boot strapping pin";
  if (p == 21 || p == 22)                     return "I2C bus (OLED/joystick)";
  if (p == 16)                                return "WS2812B data pin";
  if (p == lxpin)                             return "LX-16A bus pin";
  if (p == other)                             return "already used by the other servo";
  return nullptr;                             // pool: 4,5,13,14,17,18,19,23,25,26,27,32,33
}

// ------------------------------------------------------------
//  HALF-DUPLEX BUS ENGINE (D1 final resolution, audit finding "half-duplex recipe").
//
//  History, all MEASURED on this board (arduino-esp32 3.2.1):
//   - The lx16a-servo library flips bus direction with pinMode(pin, OUTPUT|PULLUP)
//     around every write. On core 3.x pinMode() detaches the pad from the UART via
//     the peripheral manager, so not one byte ever reached the wire.
//   - A previous TX-only workaround restored motion but left the library's bus
//     object dangling with _port == NULL; every remaining library call site was a
//     LoadProhibited crash-reboot (reproduced, backtrace-decoded).
//
//  This engine replaces the library objects ENTIRELY (none are instantiated, so a
//  null-port call cannot exist by construction). Scheme, verified against the
//  installed core source:
//   - RX is attached to the bus pin ONCE (Serial2.begin(rx=lxpin, tx=-1)) and
//     stays attached forever - it hears everything, including our own echo.
//   - TX is routed to the pad only around each packet via the GPIO matrix
//     (esp_rom_gpio_connect_out_signal), then released with gpio_set_direction
//     (GPIO_MODE_INPUT), which drops OE and de-routes the out-signal while
//     keeping the input path + pull-up intact. NEVER call pinMode on this pad -
//     it would detach the RX routing through the peripheral manager.
//   - After each send we discard exactly our own echoed bytes, then (for reads)
//     parse the servo's reply frame with checksum + id/cmd verification.
// ------------------------------------------------------------
bool lxBusUp = false;                    // half-duplex engine initialized this boot

void lxBusBegin() {                      // idempotent full (re)init of the bus engine
  Serial2.end();
  delay(5);
  Serial2.begin(115200, SERIAL_8N1, /*rx=*/lxpin, /*tx=*/-1);   // RX attached, TX unrouted
  esp_rom_gpio_pad_select_gpio((gpio_num_t)lxpin);              // pad on GPIO func (matrix out needs it)
  gpio_pullup_en((gpio_num_t)lxpin);                            // released bus idles HIGH
  gpio_set_direction((gpio_num_t)lxpin, GPIO_MODE_INPUT);       // start released: IE on, OE off
  delay(5);
  while (Serial2.available()) Serial2.read();                   // clean slate
  lxBusUp = true;
}

static inline void lxTxAttach() {        // route U2TXD -> pad (ROM call, no periman)
  esp_rom_gpio_connect_out_signal(lxpin, U2TXD_OUT_IDX, false, false);
}
static inline void lxTxRelease() {       // OE off + out-signal de-routed; RX + pull-up stay
  gpio_set_direction((gpio_num_t)lxpin, GPIO_MODE_INPUT);
}

// idFor(): map a logical axis to its live bus ID (the NVS-owned identity).
static inline uint8_t idFor(Axis a) { return (a == PAN) ? (uint8_t)panid : (uint8_t)tiltid; }

// ============================================================
//  Low-level LX-16A raw helpers (kept out of the class so config endpoints can
//  reuse them). The library checksums every packet for us - never hand-roll it.
// ============================================================

static const int     LX_BAUD         = 115200;   // LX-16A bus baud
static const uint8_t LX_BROADCAST_ID = 254;      // "all servos", regardless of programmed ID

// lxRawSend(): frame a packet (0x55 0x55 id len cmd params ~checksum), attach TX,
// push it, release the bus, then discard exactly our own echo (RX hears our TX).
static inline void lxRawSend(uint8_t id, uint8_t cmd, const uint8_t* p, uint8_t n) {
  if (!lxBusUp) return;
  uint8_t len = n + 3;
  uint8_t sum = id + len + cmd;
  for (uint8_t i = 0; i < n; i++) sum += p[i];
  uint8_t buf[16], k = 0;
  buf[k++] = 0x55; buf[k++] = 0x55;
  buf[k++] = id;   buf[k++] = len;  buf[k++] = cmd;
  for (uint8_t i = 0; i < n && k < 15; i++) buf[k++] = p[i];
  buf[k++] = (uint8_t)~sum;

  while (Serial2.available()) Serial2.read();   // drop stale bytes BEFORE tx, never after
  lxTxAttach();
  Serial2.write(buf, k);
  Serial2.flush();                              // returns when the last stop bit is on the wire
  lxTxRelease();

  uint8_t got = 0;                              // echo discard: exactly the k bytes we sent
  uint32_t t0 = millis();
  while (got < k && millis() - t0 < 4) {
    if (Serial2.available() > 0) { Serial2.read(); got++; }
  }
}

// lxRawRead(): send a read command, then parse the servo's reply frame.
// Returns the param count (>=0, params copied into out) or -1 on timeout/garbage.
// Robust against residual echo: syncs on 0x55 0x55, requires cmd (and, for
// non-broadcast requests, id) to match, and verifies the checksum.
static inline int lxRawRead(uint8_t id, uint8_t cmd, uint8_t* out, uint8_t maxN,
                            uint16_t deadlineMs = 12) {
  if (!lxBusUp) return -1;
  lxRawSend(id, cmd, nullptr, 0);
  uint8_t f[24];
  uint8_t have = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < deadlineMs) {
    int c = Serial2.read();
    if (c < 0) { delayMicroseconds(150); continue; }
    if (have == 0 && c != 0x55) continue;                 // sync byte 1
    if (have == 1 && c != 0x55) { have = (c == 0x55) ? 1 : 0; continue; }  // sync byte 2
    f[have++] = (uint8_t)c;
    if (have >= 5) {
      uint8_t flen = f[3];                                // id len cmd params chk
      if (flen < 3 || flen > 10) { have = 0; continue; }  // absurd length -> resync
      if (have == (uint8_t)(flen + 3)) {                  // full frame: 2 hdr + 1 id + len bytes
        uint8_t sum = 0;
        for (uint8_t i = 2; i < have - 1; i++) sum += f[i];
        if ((uint8_t)~sum != f[have - 1]) { have = 0; continue; }        // bad checksum -> resync
        if (f[4] != cmd)                  { have = 0; continue; }        // different command
        if (id != 254 && f[2] != id)      { have = 0; continue; }        // wrong responder
        uint8_t n = flen - 3;
        for (uint8_t i = 0; i < n && i < maxN; i++) out[i] = f[5 + i];
        return n;
      }
    }
  }
  return -1;
}

// Convenience typed reads (LE). Return -1 on failure.
static inline int lxRead16(uint8_t id, uint8_t cmd) {
  uint8_t p[4];
  return (lxRawRead(id, cmd, p, 4) >= 2) ? (p[0] | ((int)p[1] << 8)) : -1;
}
static inline int lxRead8(uint8_t id, uint8_t cmd) {
  uint8_t p[4];
  return (lxRawRead(id, cmd, p, 4) >= 1) ? p[0] : -1;
}

// lxRawMove(): the hot-path write. Angle mapping is 1:1 physical degrees:
// ticks = round(deg * 1000 / 240), logical 0..180 -> ticks 0..750; the servo's
// extra 60 deg of physical travel stays deliberately unused.
static inline void lxRawMove(uint8_t id, int deg, uint16_t ms) {
  int ticks = constrain((deg * 1000 + 120) / 240, 0, 1000);   // +120 = round to nearest
  uint8_t p[4] = { (uint8_t)(ticks & 0xFF), (uint8_t)(ticks >> 8),
                   (uint8_t)(ms & 0xFF),    (uint8_t)(ms >> 8) };
  lxRawSend(id, LX16A_SERVO_MOVE_TIME_WRITE, p, 4);
}

// Under/over-voltage cutoff window (cmd 22, mV, uint16 LE pair).
static inline void lxSetVinLimit(uint8_t id, int minMv, int maxMv) {
  uint8_t p[4] = { (uint8_t)(minMv & 0xFF), (uint8_t)(minMv >> 8),
                   (uint8_t)(maxMv & 0xFF), (uint8_t)(maxMv >> 8) };
  lxRawSend(id, LX16A_SERVO_VIN_LIMIT_WRITE, p, 4);
}

// Over-temperature cutoff (cmd 24, 1 byte deg C).
static inline void lxSetTempLimit(uint8_t id, int maxC) {
  uint8_t p[1] = { (uint8_t)maxC };
  lxRawSend(id, LX16A_SERVO_TEMP_MAX_LIMIT_WRITE, p, 1);
}

// ============================================================
//  Result structs returned to the .ino endpoints (which format the wire reply).
//  A field value of -1 means "read failed / not available" (best-effort reads).
// ============================================================
struct LxScanResult { int id; int vinMv; int tempC; int posDeg; };

struct LxConfig {
  int id;
  int alimMin, alimMax;   // hardware angle limits, ticks 0..1000     (cmd 21)
  int vlimMin, vlimMax;   // voltage limits, mV                        (cmd 23)
  int tlim;               // temperature cap, deg C                    (cmd 25)
  int mode,   speed;      // 0=servo/1=motor, motor speed -1000..1000  (cmd 30)
  int torque;             // 0=unloaded / 1=loaded                     (cmd 32)
  int trim;               // angle offset trim, -125..125              (cmd 19)
  int led;                // 0=ON / 1=OFF  (inverted logic)            (cmd 34)
  int lederr;             // alarm-enable mask 0..7 (CONFIG, not fault) (cmd 36)
  int posDeg, vinMv, tempC;  // live telemetry                    (cmd 28 / 27 / 26)
};

struct LxIdResult    { bool ok; int id; const char* msg; };   // ID-program result
struct LxWriteResult { bool ok;         const char* msg; };   // config-write result

// ------------------------------------------------------------
//  Stiction / undershoot auto-trim (user idea, 2026-08-09).
//  Measured: commanded 120 -> the (undervolted) servo lands at 118 and stall-pushes.
//  Scheme, exactly the user's staircase: after a move settles, if the servo landed
//  short, re-command past the target by the shortfall (up to 2 bumps); when it
//  finally lands, LEARN the total overshoot that was needed - per axis and per
//  DIRECTION (stiction is directional) - so the next first command already includes
//  it. Every settle/bump is recorded in a ring readable at GET /lxcal.
// ------------------------------------------------------------
static const int LX_TRIM_CAP10    = 40;  // learned trim cap, tenths of a degree (4.0 deg)
static const int LX_TRIM_APPLYCAP = 6;   // hard cap on |physical - logical| ever commanded, deg
int lxTrim10Up[2] = { 0, 0 };            // learned trim, tenths deg, moving UP   (index: PAN/TILT)
int lxTrim10Dn[2] = { 0, 0 };            // learned trim, tenths deg, moving DOWN

struct LxCalRec { uint32_t ms; int8_t axis, dir; int16_t cmd, phys, actual, err; };
LxCalRec lxCalRing[10];                  // last 10 settle observations (newest overwrites oldest)
uint8_t  lxCalHead = 0, lxCalCount = 0;
static inline void lxCalRecord(int axis, int dir, int cmd, int phys, int actual) {
  LxCalRec &r = lxCalRing[lxCalHead];
  r.ms = millis(); r.axis = (int8_t)axis; r.dir = (int8_t)dir;
  r.cmd = (int16_t)cmd; r.phys = (int16_t)phys; r.actual = (int16_t)actual;
  r.err = (int16_t)(cmd - actual);
  lxCalHead = (uint8_t)((lxCalHead + 1) % 10);
  if (lxCalCount < 10) lxCalCount++;
}

// EMA learn: grow slowly (undershoot is annoying), shrink FAST (a stale-high trim on a
// now-healthy supply would overshoot the mechanism past its logical target - safety).
static inline void lxTrimLearn(int axis, int dir, int achieved10) {
  int *b = (dir >= 0) ? &lxTrim10Up[axis] : &lxTrim10Dn[axis];
  int nb = (achieved10 >= *b) ? (7 * (*b) + 3 * achieved10) / 10
                              : (4 * (*b) + 6 * achieved10) / 10;
  *b = constrain(nb, 0, LX_TRIM_CAP10);
}

// ============================================================
//  Bus scan / discovery (section 6.1). Raw reads (no LX16AServo::initialize()
//  side effects) so a scan never rewrites a probed servo's mode.
// ============================================================

// lxScanRange(): poll IDs 1..hi with a targeted POS_READ; each responder yields
// one LxScanResult. Bounded and safe on a populated bus. Returns the count
// written into out[0..maxOut). Caller (/servoscan?mode=range) formats the TSV.
static inline int lxScanRange(int hi, LxScanResult* out, int maxOut) {
  if (hi < 1)   hi = 1;
  if (hi > 253) hi = 253;
  int n = 0;
  for (int id = 1; id <= hi && n < maxOut; id++) {
    int ticks = lxRead16(id, LX16A_SERVO_POS_READ);
    if (ticks < 0) continue;                                        // nobody home at this id
    LxScanResult r;
    r.id     = id;
    r.posDeg = ticks * 240 / 1000;                                  // raw ticks -> physical deg
    r.vinMv  = lxRead16(id, LX16A_SERVO_VIN_READ);
    r.tempC  = lxRead8(id, LX16A_SERVO_TEMP_READ);
    out[n++] = r;
  }
  return n;
}

// lxScanLone(): broadcast ID_READ - the only broadcast-capable read. Returns a
// clean ID ONLY when EXACTLY ONE servo is on the bus (two responders collide and
// the read fails/misparses). Returns 0 = none / ambiguous / read failed (also
// the value the library reports on failure; ID 0 is a discouraged config).
// Used by /servoid flow A to authenticate "exactly one servo" before broadcast.
static inline int lxScanLone() {
  int id = lxRead8(254, LX16A_SERVO_ID_READ);   // broadcast; sole responder answers with its ID
  return (id > 0 && id <= 253) ? id : 0;
}

// ============================================================
//  Safe ID programming (section 6.2). Both flows are collision-safe.
// ============================================================

// Flow A - broadcast provision (no cur; requires exactly one servo on the bus).
// Authenticate single-servo via mode=lone (NOT a bounded range sweep, which
// misses IDs 31..253), broadcast-write, then verify addressed to the new id.
static inline LxIdResult lxProgramIdBroadcast(int newId) {
  if (newId < 0 || newId > 253) return { false, newId, "bad_id" };
  int lone = lxScanLone();
  if (lone <= 0)                return { false, newId, "not_single" };  // 0 => none/ambiguous
  uint8_t np[1] = { (uint8_t)newId };
  lxRawSend(254, LX16A_SERVO_ID_WRITE, np, 1);                          // broadcast write
  delay(20);
  if (lxRead8(newId, LX16A_SERVO_ID_READ) == newId) return { true, newId, "ok" };
  return { false, newId, "verify_failed" };
}

// Flow B - targeted re-ID inside an assembled rig (cur given). id_write() in the
// library is broadcast-only, so the targeted write is issued raw. Pre-checks:
// cur must actually answer, and newId must NOT already be in use by a different
// servo (else id_verify(new) would falsely pass because *something* answers).
static inline LxIdResult lxProgramIdTargeted(int curId, int newId) {
  if (curId < 0 || curId > 253 || newId < 0 || newId > 253) return { false, newId, "bad_id" };
  if (lxRead8(curId, LX16A_SERVO_ID_READ) != curId) return { false, newId, "cur_absent" };
  if (newId != curId &&                                   // duplicate pre-check
      lxRead8(newId, LX16A_SERVO_ID_READ) == newId) return { false, newId, "dup" };
  uint8_t np[1] = { (uint8_t)newId };
  lxRawSend((uint8_t)curId, LX16A_SERVO_ID_WRITE, np, 1);  // targeted write
  delay(20);
  if (lxRead8(newId, LX16A_SERVO_ID_READ) == newId) return { true, newId, "ok" };
  return { false, newId, "verify_failed" };
}

// ============================================================
//  Per-servo config read/write + factory recovery (section 6.3).
// ============================================================

// lxFactory(): recovery - restore a working range on a servo whose bad limits
// locked it out. Widest-safe defaults: angle 0..1000 ticks, vin 4500..12000 mV,
// temp cap 85 deg C. (Defined before lxCfgWrite so it can dispatch to it.)
static inline bool lxFactory(int id) {
  if (id < 0 || id > 253) return false;
  uint8_t a[4] = { 0, 0, (uint8_t)(1000 & 0xFF), (uint8_t)(1000 >> 8) };   // 0..1000 ticks
  lxRawSend((uint8_t)id, LX16A_SERVO_ANGLE_LIMIT_WRITE, a, 4);
  lxSetVinLimit(id, 4500, 12000);
  lxSetTempLimit(id, 85);
  return true;
}

// lxCfgRead(): read every register for one servo. Best-effort - any field whose
// read fails stays -1. Caller (/servocfg?id=) formats key=value lines.
static inline LxConfig lxCfgRead(int id) {
  LxConfig c;
  memset(&c, -1, sizeof(c));        // all ints -> -1 (n/a) sentinel
  c.id = id;
  uint8_t p[6];
  if (lxRawRead(id, LX16A_SERVO_ANGLE_LIMIT_READ, p, 4) >= 4) { c.alimMin = p[0] | ((int)p[1] << 8); c.alimMax = p[2] | ((int)p[3] << 8); }
  if (lxRawRead(id, LX16A_SERVO_VIN_LIMIT_READ,   p, 4) >= 4) { c.vlimMin = p[0] | ((int)p[1] << 8); c.vlimMax = p[2] | ((int)p[3] << 8); }
  c.tlim = lxRead8(id, LX16A_SERVO_TEMP_MAX_LIMIT_READ);
  if (lxRawRead(id, LX16A_SERVO_OR_MOTOR_MODE_READ, p, 4) >= 4) { c.mode = p[0]; c.speed = (int16_t)(p[2] | ((int)p[3] << 8)); }
  c.torque = lxRead8(id, LX16A_SERVO_LOAD_OR_UNLOAD_READ);
  { int t = lxRead8(id, LX16A_SERVO_ANGLE_OFFSET_READ); c.trim = (t < 0) ? -1 : (int8_t)(uint8_t)t; }
  c.led    = lxRead8(id, LX16A_SERVO_LED_CTRL_READ);
  c.lederr = lxRead8(id, LX16A_SERVO_LED_ERROR_READ);
  { int t = lxRead16(id, LX16A_SERVO_POS_READ); c.posDeg = (t < 0) ? -1 : t * 240 / 1000; }
  c.vinMv = lxRead16(id, LX16A_SERVO_VIN_READ);
  c.tempC = lxRead8(id, LX16A_SERVO_TEMP_READ);
  return c;
}

// lxCfgWrite(): write ONE field, range-validated server-side BEFORE the packet
// is sent (un-brickable). `field` matches the plan's set= names. v2 is used only
// by two-value fields (alim / vlim / mode). Confirm torque=0 (unload) before use
// on a mounted rig - a loaded rig could drop. Returns ok + a short reason.
static inline LxWriteResult lxCfgWrite(int id, const char* field, int v, int v2 = 0) {
  if (id < 0 || id > 253) return { false, "bad_id" };

  if (!strcmp(field, "alim")) {                 // cmd 20: v=min, v2=max ticks, 0..1000, min<max
    if (v < 0 || v2 > 1000 || v >= v2) return { false, "range" };
    uint8_t p[4] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8), (uint8_t)(v2 & 0xFF), (uint8_t)(v2 >> 8) };
    lxRawSend((uint8_t)id, LX16A_SERVO_ANGLE_LIMIT_WRITE, p, 4);
    return { true, "ok" };
  }
  if (!strcmp(field, "vlim")) {                 // cmd 22: v=min, v2=max mV, 4500..12000
    if (v < 4500 || v2 > 12000 || v >= v2) return { false, "range" };
    lxSetVinLimit(id, v, v2);
    return { true, "ok" };
  }
  if (!strcmp(field, "tlim")) {                 // cmd 24: v = max deg C, 50..100
    if (v < 50 || v > 100) return { false, "range" };
    lxSetTempLimit(id, v);
    return { true, "ok" };
  }
  if (!strcmp(field, "mode")) {                 // cmd 29: v=0 servo/1 motor, v2=speed -1000..1000
    if (v < 0 || v > 1 || v2 < -1000 || v2 > 1000) return { false, "range" };
    uint8_t p[4] = { (uint8_t)v, 0, (uint8_t)(v2 & 0xFF), (uint8_t)((v2 >> 8) & 0xFF) };
    lxRawSend((uint8_t)id, LX16A_SERVO_OR_MOTOR_MODE_WRITE, p, 4);
    return { true, "ok" };
  }
  if (!strcmp(field, "torque")) {               // cmd 31: v=0 unload / 1 load
    if (v < 0 || v > 1) return { false, "range" };
    uint8_t p[1] = { (uint8_t)v };
    lxRawSend((uint8_t)id, LX16A_SERVO_LOAD_OR_UNLOAD_WRITE, p, 1);
    return { true, "ok" };
  }
  if (!strcmp(field, "trim")) {                 // cmd 17 (RAM): v = -125..125
    if (v < -125 || v > 125) return { false, "range" };
    uint8_t p[1] = { (uint8_t)(int8_t)v };
    lxRawSend((uint8_t)id, LX16A_SERVO_ANGLE_OFFSET_ADJUST, p, 1);
    return { true, "ok" };
  }
  if (!strcmp(field, "trimsave")) {             // cmd 18: persist trim to EEPROM (no params)
    lxRawSend((uint8_t)id, LX16A_SERVO_ANGLE_OFFSET_WRITE, nullptr, 0);
    return { true, "ok" };
  }
  if (!strcmp(field, "led")) {                  // cmd 33: v=0 ON / 1 OFF (inverted logic)
    if (v < 0 || v > 1) return { false, "range" };
    uint8_t p[1] = { (uint8_t)v };
    lxRawSend((uint8_t)id, LX16A_SERVO_LED_CTRL_WRITE, p, 1);
    return { true, "ok" };
  }
  if (!strcmp(field, "lederr")) {               // cmd 35: v=0..7 alarm-enable mask (config only)
    if (v < 0 || v > 7) return { false, "range" };
    uint8_t p[1] = { (uint8_t)v };
    lxRawSend((uint8_t)id, LX16A_SERVO_LED_ERROR_WRITE, p, 1);
    return { true, "ok" };
  }
  if (!strcmp(field, "factory")) {              // recovery: limits/vin/temp defaults
    return lxFactory(id) ? LxWriteResult{ true, "ok" } : LxWriteResult{ false, "factory_failed" };
  }
  return { false, "bad_field" };
}

// ============================================================
//  Section 5.1 - the seam. Base interface + the two backends.
// ============================================================
struct ServoBackend {
  virtual ~ServoBackend() {}                       // owned instances outlive boot; safe/correct
  virtual void begin() = 0;                         // owns setup()'s servo bring-up block
  virtual void writeAngle(Axis a, int deg) = 0;     // deg 0..180 logical; replaces the 6 runtime writes
  virtual void bind(Axis a, int id)   {}            // live ID re-link (no-op for PWM) - MUST be on base
  virtual bool telemetry()            { return false; }
  virtual int  readAngle(Axis a)      { return -1; } // actual pos, logical deg, -1 = n/a
  virtual int  readVinMv(Axis a)      { return -1; }
  virtual int  readTempC(Axis a)      { return -1; }
  virtual const char* name()          { return "?"; }
};

// ---- PwmBackend: the existing SG90 path, byte-for-byte behavior-preserving. ----
struct PwmBackend : ServoBackend {
  // begin() == the current setup() servo block (L807-818), including the
  // panPos/tiltPos = homePan/homeTilt assignment and the two initial writes.
  void begin() override {
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    panServo.setPeriodHertz(50);
    tiltServo.setPeriodHertz(50);
    panServo.attach(pwmPanPin,  SERVO_MIN_US, SERVO_MAX_US);
    tiltServo.attach(pwmTiltPin, SERVO_MIN_US, SERVO_MAX_US);
    panPos  = homePan;
    tiltPos = homeTilt;
    panServo.write(panPos);
    tiltServo.write(tiltPos);
  }
  void writeAngle(Axis a, int deg) override {
    (a == PAN ? panServo : tiltServo).write(deg);
  }
  const char* name() override { return "PWM"; }
  // telemetry()/readAngle()/readVinMv()/readTempC() inherit the -1 defaults.
};

// ---- Lx16aBackend: the additive serial-bus path. ----
struct Lx16aBackend : ServoBackend {
  int      lxLast[2]  = { -1, -1 };  // last commanded LOGICAL deg per axis (delta-scaled move time)
  int      lxPhys[2]  = { -1, -1 };  // last PHYSICAL deg sent (logical + learned trim + bumps)
  int8_t   lxDir[2]   = { 1, 1 };    // direction of the last logical move (+1/-1; sticky on repeats)
  uint32_t lxCmdMs[2] = { 0, 0 };    // millis of the last command per axis (settle timing)
  bool     lxRelaxed[2] = { false, false };  // torque released after idle (user option `lxrelax`):
                                             // the servo stops stall-pushing/buzzing while parked;
                                             // any new command re-engages torque first

  // begin(): half-duplex bring-up. Order: bus engine up, torque on (broadcast, so it
  // works whatever ID the servo carries), then a gentle STAGGERED glide home (audit:
  // simultaneous dual-axis torque+glide is a peak-current event that can brown out a
  // marginal supply into a boot loop). The caller (setup) sets homePan/homeTilt to THIS
  // target's own home before calling (audit: boot must not home a rig to a foreign
  // target's calibration).
  //
  // Deliberately NOT written here (audit findings):
  //  - EEPROM angle-limit widen: an EEPROM write per boot wears the servo out through
  //    crash/brownout loops and silently wipes user-configured hardware limits. Limits
  //    are now readable/editable per servo via /servocfg and the OLED Servo page.
  //  - Voltage-protection window: our 6000 mV floor + a sagging nominal-6V supply would
  //    make the servo cut its own torque constantly. Factory protection still applies.
  void begin() override {
    lxBusBegin();

    panPos  = homePan;                               // logical model parks at home on boot
    tiltPos = homeTilt;

    uint8_t on[1] = { 1 };                           // cmd 31: load torque, all servos
    lxRawSend(LX_BROADCAST_ID, LX16A_SERVO_LOAD_OR_UNLOAD_WRITE, on, 1);
    delay(100);
    lxRawMove(idFor(PAN),  homePan,  LX_HOME_MS);    // staggered: pan first...
    delay(150);
    lxRawMove(idFor(TILT), homeTilt, LX_HOME_MS);    // ...then tilt
    delay(LX_HOME_MS);

    lxLast[PAN]  = homePan;
    lxLast[TILT] = homeTilt;
  }

  // Hot path: raw MOVE packet with move time scaled by the commanded delta (5.4),
  // so a 1-deg glide step is smooth and a 40-deg reclamp yank isn't current-spiky.
  // The learned per-direction trim is applied here, so the FIRST command already
  // overshoots by what this servo has been measured to need; the physical command
  // never strays more than LX_TRIM_APPLYCAP from the logical target.
  void writeAngle(Axis a, int deg) override {
    int i = (a == PAN) ? 0 : 1;
    int d = (lxLast[i] < 0) ? 0 : deg - lxLast[i];
    if (d > 0) lxDir[i] = +1; else if (d < 0) lxDir[i] = -1;    // repeats keep the prior direction
    int b10  = (lxDir[i] >= 0) ? lxTrim10Up[i] : lxTrim10Dn[i];
    int phys = deg + (int)lxDir[i] * ((b10 + 5) / 10);
    phys = constrain(phys, deg - LX_TRIM_APPLYCAP, deg + LX_TRIM_APPLYCAP);
    phys = constrain(phys, 0, 180);
    int ms = constrain(LX_MS_K * abs(d), LX_MS_FLOOR, LX_MS_CEIL);
    if (lxRelaxed[i]) {                  // waking a released axis: torque on first, and move
      uint8_t on[1] = { 1 };             // gently - it may have drifted while limp, and the
      lxRawSend(idFor(a), LX16A_SERVO_LOAD_OR_UNLOAD_WRITE, on, 1);   // delta-scaled ms only
      lxRelaxed[i] = false;              // reflects the LOGICAL delta, not the physical one
      if (ms < 250) ms = 250;
    }
    lxRawMove(idFor(a), phys, ms);       // raw send: the library's write path never reaches the pad (D1)
    lxLast[i]  = deg;
    lxPhys[i]  = phys;
    lxCmdMs[i] = millis();
  }

  // Accept-reality park (user rule): when a settled move can't close the last <=2 deg,
  // adopt the measured position as the target - re-command AT the actual, with no trim,
  // so the internal controller stops leaning on a gap it cannot close (the buzz).
  void parkAt(Axis a, int deg) {
    int i = (a == PAN) ? 0 : 1;
    deg = constrain(deg, 0, 180);
    lxRawMove(idFor(a), deg, 120);
    lxLast[i]  = deg;
    lxPhys[i]  = deg;
    lxCmdMs[i] = millis();
  }

  // Idle torque release (user option): stop the hold-controller's stall-push/buzz while
  // parked. Position keeps being read; the next writeAngle re-engages torque.
  void relax(Axis a) {
    int i = (a == PAN) ? 0 : 1;
    if (lxRelaxed[i]) return;
    uint8_t off[1] = { 0 };
    lxRawSend(idFor(a), LX16A_SERVO_LOAD_OR_UNLOAD_WRITE, off, 1);
    lxRelaxed[i] = true;
  }

  // Staircase settle bump (auto-trim): push the PHYSICAL command further by the
  // measured shortfall, without ever drifting more than LX_TRIM_APPLYCAP from the
  // logical target. Called from updateLxTrim() once the previous move has settled.
  void bumpPhys(Axis a, int delta) {
    int i = (a == PAN) ? 0 : 1;
    if (lxPhys[i] < 0 || lxLast[i] < 0) return;
    int phys = constrain(lxPhys[i] + delta,
                         lxLast[i] - LX_TRIM_APPLYCAP, lxLast[i] + LX_TRIM_APPLYCAP);
    phys = constrain(phys, 0, 180);
    if (phys == lxPhys[i]) return;                  // cap reached: nothing more to give
    lxRawMove(idFor(a), phys, 150);
    lxPhys[i]  = phys;
    lxCmdMs[i] = millis();
  }

  // Live ID re-link (section 6.4). Raw reads address by explicit ID (idFor), so the
  // .ino updating panid/tiltid + NVS is already the whole re-link; nothing cached here.
  void bind(Axis a, int id) override { (void)a; (void)id; }

  // Reads are live again (half-duplex engine). Best-effort: -1 = no/garbled reply,
  // and the caller (updateTelemetry / deriveFault) treats -1 as "discard".
  bool telemetry() override { return true; }

  int readAngle(Axis a) override {
    int ticks = lxRead16(idFor(a), LX16A_SERVO_POS_READ);
    return (ticks < 0) ? -1 : ticks * 240 / 1000;     // ticks 0..1000 -> physical deg
  }
  int readVinMv(Axis a) override { return lxRead16(idFor(a), LX16A_SERVO_VIN_READ); }
  int readTempC(Axis a) override { return lxRead8(idFor(a),  LX16A_SERVO_TEMP_READ); }

  const char* name() override { return "LX16A"; }
};
