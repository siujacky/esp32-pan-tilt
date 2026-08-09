# Plan: LX-16A Serial-Bus Servo Support for the ESP32 Pan/Tilt Controller

Status: **design / ready to execute.** Target files: `platformio.ini`, new `esp32_pan_tilt/servo_backend.h`, `esp32_pan_tilt/esp32_pan_tilt.ino`, `esp32_pan_tilt/web_page.h`.

This document supersedes the initial design draft; every correction raised in the two adversarial reviews (hardware/protocol and software/UX) has been folded in. A change log of those fixes is in [Appendix A](#appendix-a--review-corrections-incorporated).

---

## 1. Overview & goals

### 1.1 Design thesis

The firmware is already written entirely in a backend-agnostic **0–180 integer-degree model** (`panPos` / `tiltPos`, all soft limits, home, glide, OLED, LEDs). The only PWM-coupled surface is the **8 `Servo::write()` calls** in three runtime functions plus **one bring-up block** in `setup()`. We introduce a thin `ServoBackend` interface, wrap those sites behind it, add an `Lx16aBackend` alongside the existing PWM one, and select between them at boot from a single NVS key. Everything else — nudge, glide, soft limits, home, LEDs, OLED, WiFi, and the CSV status contract — is reused unchanged.

Default `bknd = 0` (PWM) means the refactor ships with **zero behavior change** until the user opts in.

### 1.2 Goals

| # | Goal |
|---|---|
| G1 | Add LX-16A serial-bus servo support without regressing the existing SG90/PWM build. |
| G2 | Both servos share **one data wire**, addressed by ID — the core topology change from two PWM pins. |
| G3 | Select backend at boot via NVS; PWM remains the default. |
| G4 | Reuse the entire logical model (limits/home/glide/invert) and all existing UI/OLED/LED behavior. |
| G5 | Expose the serial bus's new capabilities on the web UI: bus scan, safe ID programming, full per-servo config read/write, ID→axis linking, and live telemetry (voltage / temperature / actual position / fault). |
| G6 | Preserve the CSV status contract by **appending only**; no existing field moves or changes meaning. |
| G7 | Make every servo-mutating web action collision-safe and un-brickable (validated + recoverable). |

### 1.3 Non-goals (v1)

- Widening the logical model beyond 0–180° (the LX-16A's extra 60° of its 240° range stays unused — see [§5.3](#53-angle-mapping-0180-ui--lx-16a-0240--01000-ticks)).
- Continuous-rotation ("motor") mode as a primary control path (the register is exposed in config, but the D-pad model stays position-based).
- Caching each servo's EEPROM configuration in NVS (the servo is the source of truth — see [§8](#8-persistence-nvs-namespace-pantilt)).

---

## 2. LX-16A background

The **HiWonder / LewanSoul LX-16A** is a serial *bus* servo: instead of a PWM pulse per servo, a half-duplex asynchronous serial line carries addressed command packets, so many servos share one signal wire and each is selected by a 1-byte ID.

### 2.1 Electrical / mechanical facts

| Property | Value | Consequence for this project |
|---|---|---|
| Operating voltage | **6.0–8.4 V** (2S LiPo nominal 7.4 V) | **Cannot** run on the 5 V SG90/logic rail — needs a separate supply. |
| Logic level | 3.3 V internally | GPIO17 connects directly; **no level shifter**. |
| Baud | **115200**, 8N1, fixed | Set inside the library; independent of the USB console (UART0). |
| Bus | Half-duplex, one signal wire | Both servos daisy-chain onto one GPIO. |
| Physical range | **0–240°** ↔ **0–1000 "ticks"** | Position command/limit unit is ticks; see mapping [§5.3](#53-angle-mapping-0180-ui--lx-16a-0240--01000-ticks). |
| Stall current | ~1–2 A per servo | Bulk cap + fuse on the servo rail (see wiring). |
| ID range | **0–253** valid; **254 (0xFE)** = broadcast | Factory default ID = **1** (two stock servos collide!). |

### 2.2 Packet framing

```text
0x55 0x55  ID  Length  Command  Param1 … ParamN  Checksum
           │    │        │                          │
           │    │        │                          └ ~(ID+Length+Command+Params) & 0xFF
           │    │        └ command number (table below)
           │    └ Length = N_params + 3
           └ target servo ID, or 0xFE broadcast
```

The chosen library computes the checksum for us (`write_no_retry` internally does `~sum`), so **never hand-roll it** — even the raw-write hot path (see [§5.4](#54-hot-path-write--bypass-the-wrapper)) delegates checksumming to the library.

### 2.3 Half-duplex on the ESP32 — how the library actually does it

> **Correction vs the initial draft.** This is **not** open-drain on the ESP32. The library transmits by driving the pin `OUTPUT | PULLUP` (push-pull), then flips it to `INPUT | PULLUP` to listen; RX and TX are both matrixed onto the single pin via `Serial2.begin(baud, SERIAL_8N1, pin, pin)`. Only the Teensy branch uses `OUTPUT_OPENDRAIN`. So the mechanism is **software half-duplex direction-flipping with push-pull drive.**

Two practical consequences drive the wiring and the telemetry design:

1. **Bus contention is a driver-vs-driver near-short.** Because TX is push-pull, any contention (the flip-timing race, or two un-provisioned servos both replying) shorts one driver against another. A **series resistor on the data line** limits that fault current — this is the library author's *recommended* method, not an optional extra (see [§4.3](#43-signal-line-protection)).
2. **Every transmitted byte is echoed back on the shared pin** (local loopback). The echo is a well-formed `0x55 0x55 …` packet with a valid checksum, so a read can occasionally validate on its own outgoing echo and return garbage params. **Reads are therefore best-effort by design** — this shapes the entire telemetry/fault strategy ([§5.5](#55-reads-are-best-effort-critical), [§6.5](#65-live-telemetry), [§10](#10-safety)).

### 2.4 Command set used by this project

| Cmd (W/R) | Name | Used for |
|---|---|---|
| 1 | `MOVE_TIME_WRITE` | Move to ticks over N ms — the hot path |
| 13 / 14 | `ID_WRITE` / `ID_READ` | Program / discover ID |
| 17 / 18 / 19 | `ANGLE_OFFSET_ADJUST` / `_WRITE` / `_READ` | Trim (RAM / EEPROM / read) |
| 20 / 21 | `ANGLE_LIMIT_WRITE` / `_READ` | **Hardware** end-stops (ticks, EEPROM) |
| 22 / 23 | `VIN_LIMIT_WRITE` / `_READ` | Under/over-voltage cutoff |
| 24 / 25 | `TEMP_MAX_LIMIT_WRITE` / `_READ` | Over-temperature cutoff |
| 26 | `TEMP_READ` | Telemetry: temperature °C |
| 27 | `VIN_READ` | Telemetry: input voltage mV |
| 28 | `POS_READ` | Telemetry: actual position |
| 29 / 30 | `OR_MOTOR_MODE_WRITE` / `_READ` | Servo vs continuous-motor mode |
| 31 / 32 | `LOAD_OR_UNLOAD_WRITE` / `_READ` | Torque enable/disable |
| 33 / 34 | `LED_CTRL_WRITE` / `_READ` | LED on/off (**inverted logic**) |
| 35 / 36 | `LED_ERROR_WRITE` / `_READ` | Which alarms *may* light the LED — **configuration only, not live status** |

> **Correction vs the initial draft.** Cmd 35/36 is the **alarm-enable configuration** (which conditions are allowed to light the servo's LED), stored in EEPROM. Reading cmd 36 returns *what you wrote*, not the *currently-active* fault. The LX-16A has **no live-fault register.** Faults are therefore *derived in firmware* from temperature/voltage/position telemetry — see [§9.3](#93-derived-fault-state).

---

## 3. Library choice

**Chosen: [`madhephaestus/lx16a-servo`](https://github.com/madhephaestus/lx16a-servo) (Arduino / ESP32).**

It is the only cross-checked source that targets Arduino/ESP32 *and* implements the ESP32 half-duplex one-pin mode natively (`LX16ABus::beginOnePinMode(&Serial2, pin)`). The other two researched implementations are **references only**:

| Library | Language / target | Role here |
|---|---|---|
| `madhephaestus/lx16a-servo` | Arduino / ESP32 | **Used.** One-pin + two-pin (buffer) modes, high-level `LX16AServo` wrappers for config/reads. |
| `maximkulkin/lewansoul-lx16a` | Python | Reference for protocol/units cross-check. |
| `SpaceMaster85/lewansoul_lx16a` | C++ / Linux | Reference only. **Do not** copy its `enableAll/disableAll` — they send cmd 13 (ID-write), not cmd 31 (torque), which would rewrite every servo's ID. |

### 3.1 Library quirks to design around (verified against source)

These are load-bearing; each is neutralized in the design below.

| Quirk | Impact | Mitigation |
|---|---|---|
| `move_time()` `Serial.println`s on every call and runs lazy `initialize()` + limit re-clamp | Noisy + slow on the glide hot path | Send the raw MOVE packet via `lxMove()` ([§5.4](#54-hot-path-write--bypass-the-wrapper)) |
| One-pin **writes** never wait for an ack → always "succeed" | Can't detect a dead bus from writes alone | Detect liveness via periodic **reads** + a consecutive-failure counter ([§9.3](#93-derived-fault-state)) |
| `pos_read()` returns the **cached last-good value on failure** | Stale-but-plausible position displayed | Gate every read on `isCommandOk()` ([§5.5](#55-reads-are-best-effort-critical)) |
| Lazy `initialize()` on first `pos_read/vin/temp` runs `readLimits()`, which has its **own** `numFail++ < 3` retry loop that **ignores** `retry = 0` | First read of each servo blocks ~120–240 ms inside `loop()` | **Prime** each servo in `begin()` before the main loop starts ([§5.6](#56-begin--priming-and-hardware-limits)) |
| `readLimits()` / `setLimitsTicks()` also `Serial.println` on every call | Console spam + latency | Call them only in `begin()` / config paths, never on the hot path |
| `id_write()` (both `LX16ABus` and `LX16AServo`) hardcodes the **broadcast** ID | Cannot target a specific current ID via the helper | Use raw `servoBus.write(LX16A_SERVO_ID_WRITE, {new}, 1, cur)` for the targeted path ([§6.2](#62-program-a-servos-id-safely)) |
| `LX16AServo::calibrate()` contains `while(1);` on `min ≥ max` and an unbounded `do{…}while(!isCommandOk())` | Can hard-hang the MCU | **Never call `calibrate()`**; use `setLimitsTicks()` directly |

### 3.2 Toolchain — platform vs. core

> **Correction vs the initial draft.** The draft pinned `platform = espressif32@2.0.0` and called it "core known-good," conflating the **PlatformIO platform** version with the **arduino-esp32 core** version. They are different numbering schemes:

| PlatformIO `espressif32` | arduino-esp32 core |
|---|---|
| 4.0.0 | 2.0.0 |
| 3.5.0 | **1.0.6** |
| 2.0.0 | ~1.0.4 / 1.0.5 (2019) |

There is **no** arduino-esp32 "core 4.0.0"; the core line is 1.0.x → 2.0.x → 3.0.x, so "broken on core 4.0.0+" was never a real version. The one-pin direction-flip was validated on the **core 1.0.x** era. If you pin for that, the correct pin is **`platform = espressif32@3.5.0` (core 1.0.6)** — *not* `@2.0.0`, which drops you to a 2019 core that predates much of the current U8g2 / Adafruit_NeoPixel / ESP32Servo / WebServer API surface and risks breaking the firmware that already works today.

**Decision path (see [§13](#13-open-decisions--risks), open decision D1):**

- **Option A — one-pin on a pinned core.** `platform = espressif32@3.5.0`, then do a **full rebuild + on-hardware regression of the existing PWM/OLED/LED/WiFi build** before touching servo code. Cheapest wiring (1 GPIO), most fragile toolchain.
- **Option B — buffered two-pin (74HC126).** Core-version-agnostic and immune to the echo/flip reliability problem, at the cost of 3 GPIOs and a WS2812B pin move (see [§4.4](#44-buffer-mode-fallback-74hc126)). Recommended if Phase 0 read-reliability is poor.

```ini
[platformio]
src_dir = esp32_pan_tilt

[env:esp32dev]
platform = espressif32@3.5.0   ; core 1.0.6 — pin ONLY if using one-pin mode (Option A); regress the whole build
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    madhephaestus/ESP32Servo         ; keep — PWM backend / coexistence
    madhephaestus/lx16a-servo        ; NEW
    olikraus/U8g2
    adafruit/Adafruit NeoPixel
```

---

## 4. Hardware & wiring

### 4.1 Current GPIO map (confirmed in `esp32_pan_tilt.ino`)

| GPIO | Use | Line |
|---|---|---|
| 25 | Pan servo PWM | L36 |
| 26 | Tilt servo PWM | L37 |
| 21 | OLED SDA | L60 |
| 22 | OLED SCL | L61 |
| 2 | Status LED | L52 |
| 16 | WS2812B corner LEDs | L70 |

### 4.2 LX-16A bus pin: **GPIO 17**

Free, UART-capable (`Serial2` default TX). One-pin half-duplex via `servoBus.beginOnePinMode(&Serial2, 17)`.

- **GPIO16 (WS2812B) stays free.** `beginOnePinMode` passes the *same* pin as both RX and TX (verified in library source), so `Serial2`'s default RX (GPIO16) is **not** claimed — no conflict with the NeoPixel. (This is specific to *one-pin* mode; the two-pin buffer path does **not** get this for free — see [§4.4](#44-buffer-mode-fallback-74hc126).)
- **Board caveat.** GPIO16/17 are the PSRAM interface on **ESP32-WROVER** modules. The project already drives GPIO16 (WS2812B), which proves this board is **not** a WROVER (or has PSRAM disabled), so 17 is safe. Confirm the module (open decision D2). If ever swapped to a WROVER, use an alternate free UART-capable pin: **4, 13, 18, 19, 23, 27, 32, or 33** (this list correctly excludes strapping pin 12 and the input-only 34–39).

The topology change in one line: PWM's two independent pins (25/26) collapse to **one shared bus on GPIO17 plus two IDs** (`panid` / `tiltid`).

### 4.3 Signal-line protection

> **Correction vs the initial draft.** The draft said "open-drain one-pin mode needs no resistor" and to avoid the 1 kΩ value. Both are backwards. The ESP32 path is push-pull half-duplex ([§2.3](#23-half-duplex-on-the-esp32--how-the-library-actually-does-it)); the library author's README specifies a **1 kΩ series resistor** on the MCU TX/data line ("MCU TX connects through a 1K Ohm resistor to the LX-16A Serial Pin"), warning that a direct connection "will get a few failed commands and errored bytes."

- **1 kΩ series resistor** on GPIO17 → servo signal (limits contention current to ~3 mA; 100–220 Ω would permit ~15 mA — do not use). This is for contention/fault-current, **not** level translation.
- The internal `PULLUP` is weak (~45 kΩ). On anything but very short leads, add an **external ~4.7 kΩ pull-up to 3.3 V** on the signal line for a clean idle at 115200.
- Signal is 3.3 V on both sides → **no level shifter** (this part of the draft was correct).

### 4.4 Recommended wiring (one-pin mode)

```text
                       ┌──── 1 kΩ series ───┐
ESP32 GPIO17 ──────────┤                    ├──► LX-16A signal (BOTH servos, daisy-chained)
                       └── 4.7 kΩ ↑3.3 V ───┘   (external pull-up for clean idle)

6.0–8.4 V supply (+) ──[ ~3–5 A fuse ]────────► LX-16A power pin (BOTH servos)
                        └─[ ~1000 µF ]─┐          (bulk cap across the rail, near the servos)
Common GND ───────────────────────────┴───────► LX-16A GND (both) ── ESP32 GND ── 5 V-rail GND
```

| Rail | Voltage | Notes / warnings |
|---|---|---|
| Servo power | **6.0–8.4 V** separate rail (2S LiPo ~7.4 V, or bench PSU) | **NOT** the 5 V SG90/logic rail — the LX-16A will not run on 5 V. **Never exceed 8.4 V.** |
| Logic | 3.3 V (ESP32) | Signal is 3.3 V both sides. |
| Ground | Shared **star point** | The two rails share **ground only**; **never** tie 7.4 V into the 5 V rail. |

**Protection checklist:** (a) common-ground star point — **mandatory**; (b) ~1000 µF bulk cap across the 6–8.4 V rail near the servos (stall spikes >1 A each); (c) inline ~3–5 A fuse on the LiPo/PSU positive; (d) 1 kΩ series resistor + 4.7 kΩ pull-up on the signal line ([§4.3](#43-signal-line-protection)); (e) never exceed 8.4 V.

### 4.5 Buffer-mode fallback (74HC126)

> **Correction vs the initial draft.** The draft's fallback pins (RX=17, TX=18, flag=19) do **not** work against the stock library. The library's two-pin `begin()` on ESP32 calls `port->begin(_baud)` with **no pin arguments**, forcing `Serial2`'s hardware defaults **RX=GPIO16, TX=GPIO17** — which re-introduces the exact GPIO16/WS2812B conflict that one-pin mode avoids.

If Phase 0 shows one-pin reads are too unreliable, adopt the buffered path **correctly**:

1. **Relocate the WS2812B off GPIO16** first (e.g. to GPIO 4 or 27; one-line change at `PIN_LEDS`, L70), *or* patch the library to accept remapped pins.
2. Wire the transceiver so the UART always listens and only drives the bus when the flag is asserted:

```text
Serial2 RX (GPIO16) ◄──────────────── servo signal      (always listening)
Serial2 TX (GPIO17) ───► 74HC126 'A'
flag GPIO (e.g. 18) ───► 74HC126 'OE'
                         74HC126 'Y' ─► servo signal
```

3. Call `servoBus.begin(&Serial2, /*txPin=*/17, /*flagPin=*/18)`.

Costs 3 GPIOs (vs 1) plus the NeoPixel move, but is core-version-agnostic and immune to the echo/flip read problem.

---

## 5. Servo-backend abstraction + angle mapping

### 5.1 The seam

New header **`esp32_pan_tilt/servo_backend.h`**, included from the `.ino` next to `#include "web_page.h"` (L21). Move `enum Axis` into this header so the interface can reference it, and delete the duplicate at `.ino` L82 (the "must precede the first function" constraint is still satisfied because the header is included before any function).

```cpp
// servo_backend.h
enum Axis { PAN, TILT };

struct ServoBackend {
  virtual void begin() = 0;                       // owns setup() L807–818 (attach/period + home write)
  virtual void writeAngle(Axis a, int deg) = 0;   // deg 0..180 logical; replaces the 6 runtime writes
  virtual void bind(Axis a, int id)   {}          // live ID re-link (no-op for PWM)  ← base must declare it
  virtual bool telemetry()            { return false; }
  virtual int  readAngle(Axis a)      { return -1; } // actual pos, logical deg, -1 = n/a
  virtual int  readVinMv(Axis a)      { return -1; }
  virtual int  readTempC(Axis a)      { return -1; }
  virtual const char* name()          { return "?"; }
};
```

> **Correction vs the initial draft.** (1) `bind()` **must** be declared on the base `ServoBackend`, or `backend->bind(...)` fails to compile when `backend` is a `PwmBackend*`. (2) `readFault()` is **removed from the backend interface** — faults are not a servo register; they are computed in firmware ([§9.3](#93-derived-fault-state)) from the vin/temp/actual values the backend already exposes.

A single global `ServoBackend* backend;` is chosen in `setup()` from the `bknd` NVS key.

### 5.2 The two backends and the write-site accounting

> **Correction vs the initial draft.** The draft said `writeAngle` replaces "8 `.write()` sites … including setup L817/818" *and* that `begin()` replaces "L807–822" (which already contains L817/818) — double-counting. Precise accounting:

- **6 runtime `writeAngle()` sites:** `updateHoming` L248/249, `reclampToLimits` L266/268, `nudge` L328/334. Each already computes the clamped degree into `panPos`/`tiltPos` *before* writing, so soft limits / home / glide / invert all live **above** the write layer and are untouched.
- **1 `begin()` block:** owns `setup()` **L807–818** — the four `allocateTimer`s, `setPeriodHertz`, both `attach`, **the `panPos/tiltPos = homePan/homeTilt` assignment (L815–816)**, and the two initial `write`s (L817–818). Note `begin()` is responsible for that home assignment; it is not a `.write()` and is easy to drop on the floor.

| Backend | `begin()` | `writeAngle(a, deg)` | Telemetry |
|---|---|---|---|
| `PwmBackend` | current timer/period/attach/home-write block | `(a==PAN?panServo:tiltServo).write(deg)` | all `-1` (defaults) |
| `Lx16aBackend` | open bus, `retry=0`, prime servos, set hardware limits, enable torque, gentle home | `lxMove(idFor(a), deg, ms)` ([§5.4](#54-hot-path-write--bypass-the-wrapper)) | `pos_read`/`vin`/`temp`, gated on `isCommandOk()` |

### 5.3 Angle mapping (0–180 UI ↔ LX-16A 0–240° / 0–1000 ticks)

**Decision: 1:1 physical degrees.** Logical `deg` maps to the *same physical angle*: `ticks = round(deg × 1000 / 240)`, so logical 0–180 → ticks 0–750 (physical 0–180°). The servo's extra 60° (180–240°) is simply unused.

Rationale (endorsed by both reviews): a logical degree stays a physical degree, so `panMin/panMax/homePan/…` keep their exact meaning across an SG90↔LX-16A swap, and both `oledBar()`'s hardcoded `/180` (L580/581/584; L578 uses `SERVO_MAX_DEG - SERVO_MIN_DEG`) and `web_page.h`'s `min=0 max=180` inputs stay correct with **no changes**.

Two distinct "limit" concepts must not be conflated:

| Concept | Where | Unit | Changes? |
|---|---|---|---|
| **Firmware soft limits** (`panMin…`) | RAM + NVS, enforced by existing `constrain()` | logical 0–180 | unchanged |
| **LX-16A hardware angle limits** (cmd 20/21) | servo EEPROM | 0–1000 ticks | new read/write config ([§6.3](#63-readwrite-all-config-per-servo)); `begin()` must widen them ([§5.6](#56-begin--priming-and-hardware-limits)) |

`INVERT_PAN/TILT` stays at the angle layer (negates the *delta* in `nudge`, L325/331) and is backend-agnostic — no change.

### 5.4 Hot-path write — bypass the wrapper

The library's `move_time()` `Serial.println`s and runs lazy init on every call. Send the raw MOVE packet instead, with the **move time scaled by the commanded delta**:

```cpp
// LX_MS_FLOOR ~40 ms (glide steps), LX_MS_CEIL ~220 ms (big jumps), LX_MS_K ~6 ms/deg
static int lxLast[2] = { -1, -1 };

void lxMove(uint8_t id, int deg, uint16_t ms) {
  int ticks = constrain((deg * 1000 + 120) / 240, 0, 1000);   // 0..180 -> 0..750
  uint8_t p[4] = { (uint8_t)(ticks & 0xFF), (uint8_t)(ticks >> 8),
                   (uint8_t)(ms & 0xFF),    (uint8_t)(ms >> 8) };
  servoBus.write(LX16A_SERVO_MOVE_TIME_WRITE, p, 4, id);       // cmd 1; checksum done by library
}

void Lx16aBackend::writeAngle(Axis a, int deg) {
  int i  = (a == PAN) ? 0 : 1;
  int d  = (lxLast[i] < 0) ? 0 : abs(deg - lxLast[i]);
  int ms = constrain(LX_MS_K * d, LX_MS_FLOOR, LX_MS_CEIL);    // delta-scaled move time
  lxMove(idFor(a), deg, ms);
  lxLast[i] = deg;
}
```

> **Correction vs the initial draft.** A single fixed `LX_MOVE_MS ≈ 60 ms` is smooth for a 1° glide step but is ~500°/s for a 30° nudge and up to ~700°/s for a full-range `reclampToLimits` correction (a tightened limit can yank the live position 40°+ in one `writeAngle`) — abrupt and current-spiky under load. **Scale the move time by `|deg − lastDeg|`** as above (a real tuning tension, open decision D6), rather than one constant.

Use the high-level `LX16AServo` wrappers only for **reads/config** (off the hot path).

### 5.5 Reads are best-effort (critical)

Because of the echo-loopback described in [§2.3](#23-half-duplex-on-the-esp32--how-the-library-actually-does-it), reads intermittently fail or misparse. The design must **tolerate** failure, not assume success:

- **Always gate on `isCommandOk()`.** `pos_read()`/`vin()`/`temp()` return the cached last-good value on failure, so an ungated read shows stale-but-plausible data. Discard the sample when `!isCommandOk()`.
- **`retry = 0`** (one attempt). A failed read then costs one ~30 ms timeout instead of 3×.
- **Never** raise a fault / "bus dead" on a *single* failed read — require **N consecutive failures** (3–5) before latching `LED_FAULT` ([§9.3](#93-derived-fault-state)), or the LED strobes red constantly.
- **Units.** Write path uses **ticks** (0–1000 = 0–240°). Read path: per the library source, `pos_read()` returns **centidegrees**, so `readAngle = pos_read() / 100` → physical degrees (= logical degrees in the 0–180 band). **Verify this on the bench in Phase 0** (open decision D-verify): if `pos_read()` actually returns raw ticks, use `readAngle = pos_read() * 240 / 1000` instead. Keep read/write symmetric once confirmed.

### 5.6 `begin()` — priming and hardware limits

`Lx16aBackend::begin()` must, **before** `server.begin()` / the main loop:

1. `servoBus.beginOnePinMode(&Serial2, LX_PIN); servoBus.retry = 0;`
2. **Prime each servo** — one `pos_read()` + one `readLimits()` per servo — so the lazy `initialize()` heavy path (which ignores `retry=0` and can block ~120–240 ms) never fires from the telemetry round-robin later.
3. **Widen the hardware angle limits:** `setLimitsTicks(0, 1000)` on each servo (or at least span the used 0–750). Otherwise a servo previously configured to, say, 0–500 ticks silently under-travels when `lxMove` commands 750, and the soft-limit UI (logical 0–180) can't explain why. **Never call `calibrate()`** ([§3.1](#31-library-quirks-to-design-around-verified-against-source)).
4. Set conservative protection windows (vin ~6000–8400 mV, temp cap ~70 °C).
5. **Gentle home:** read the current position first, then `move_time` to home over ~800 ms *before* enabling continuous control, so torque-on doesn't snap a back-driven servo.
6. Enable torque (cmd 31).

**Timing budget (honest):** writes in one-pin mode block on `flush()` (~0.9 ms for 10 bytes) plus draining the previous transaction's echo (each drained byte gated ~260 µs) → **~3–4 ms per servo, ~6–8 ms per glide tick** (not the ~1 ms the draft implied). Fine at the delta-scaled cadence, but it eats into `server.handleClient()` responsiveness during fast glides. **Reads block up to ~30 ms** and must never run on the glide/nudge path — only in the throttled telemetry task ([§9.2](#92-telemetry-task)), and never during motion.

---

## 6. Web feature set

> **Correction vs the initial draft.** These controls are **not** mostly reuse of the existing `data-cmd`/`data-set` conventions. The existing `input[data-set]` auto-wiring (L330) matches `<input>` only — **not `<select>`** — so a populated ID dropdown wired with `data-set` would silently never fire. And the four new endpoints below return TSV / `key=value` / `ok\t…`, **not** the status CSV, so their replies must **not** reach `applyState`. The real precedent is the **WiFi provisioning block** (`web_page.h` L352–385): bespoke `fetch()` handlers with their own response parsers, dropdown population, and `confirm()` dialogs. Treat the servo web UI as its own substantial work item (its own line in [§12](#12-phased-implementation-plan) and [§14](#14-files-touched)), modeled on that block.

All new controls live in new `.card`s inside the hidden advanced panel `#adv` (`web_page.h` L164–230) and are hidden when CSV field 14 (`bknd`) ≠ 1.

### 6.1 Bus scan / ID discovery — `GET /servoscan`

| Mode | Query | Behavior |
|---|---|---|
| range | `?mode=range&hi=<N>` (default N=30) | Poll IDs 1..N with a targeted `POS_READ`; each responder → one TSV line `id⇥vin_mV⇥temp_C⇥pos_deg`. Bounded, safe on a populated bus. |
| lone | `?mode=lone` | Broadcast `ID_READ` — returns the ID of the **single** servo on the bus (the only broadcast-capable read). UI must state "exactly one servo connected." |

### 6.2 Program a servo's ID safely — `GET /servoid?new=<0-253>&confirm=1[&cur=<id>]`

> **Correction vs the initial draft.** (1) A bounded range sweep (`hi=30`) can't prove "single servo" — a servo at ID 31–253 is invisible to it, so the firmware would wrongly conclude "single" and broadcast-write, re-IDing the unseen servo too. Authenticate "exactly one" with **`mode=lone`** (broadcast `ID_READ` returns cleanly only with one servo) or a **full 0..253** sweep. (2) `id_write()` is **broadcast-only** in the library; the targeted path must issue the raw packet. (3) The blanket ">1 → refuse" over-blocks the *safe* targeted case.

Two flows, each collision-safe:

**A — Broadcast provision (no `cur`, one servo on the bus):**
1. Require `confirm=1`.
2. Authenticate single-servo via `mode=lone` (**not** a bounded range sweep). If more than one servo answers / the lone-read is ambiguous → **refuse**.
3. Broadcast-write the new ID via `id_write(new)`.
4. Verify with `id_verify()` addressed to `new`.
5. Return `ok⇥<new>` or `fail⇥<reason>`.

**B — Targeted re-ID inside an assembled rig (`cur` given):**
1. Require `confirm=1`.
2. Pre-check `new ∉ (existingIDs \ {cur})` via a scan (so `cur=1→new=2` can't silently duplicate an existing ID 2 — `id_verify(new)` would falsely "pass" because *something* answers at 2).
3. Issue the **raw** targeted write: `servoBus.write(LX16A_SERVO_ID_WRITE, {new}, 1, cur)`.
4. Verify `id_verify(new)`; return `ok`/`fail`.

UI gates flow A behind a "only one servo connected" checkbox + `confirm()` dialog; flow B behind the duplicate pre-check.

### 6.3 Read/write all config per servo — `GET /servocfg`

- **Read:** `?id=<id>` → `key=value` lines for every register.
- **Write one field:** `?id=<id>&set=<field>&v=<n>[&v2=<n>]`, **range-validated server-side** before the packet is sent.
- **Recover:** `?id=<id>&set=factory` → restore angle-limit 0–1000 and vin/temp defaults (rescues a servo whose bad limits locked it out of its working range).

| Field (`set=`) | Cmd (W/R) | Params | Range / notes |
|---|---|---|---|
| `alim` | 20/21 | v=min, v2=max ticks | 0–1000, min<max (`setLimitsTicks`) |
| `vlim` | 22/23 | v=min, v2=max mV | 4500–12000 (raw write, uint16 LE pair) |
| `tlim` | 24/25 | v=°C | 50–100, default 85 (raw write) |
| `mode` | 29/30 | v=0 servo / 1 motor, v2=speed | speed −1000..1000 (`motor_mode`) |
| `torque` | 31/32 | v=0 unload / 1 load | `disable()` / `enable()` — confirm before unloading a mounted rig |
| `trim` | 17 (RAM) / 19 (R) | v=−125..125 | RAM trim (`angle_offset_adjust`) |
| `trimsave` | 18 | — | persist trim to EEPROM |
| `led` | 33/34 | v=0 ON / 1 OFF | **inverted logic** (raw write) |
| `lederr` | 35/36 | v=0..7 | 1=temp, 2=volt, 4=stall — **alarm config only**, not live fault |
| `factory` | 20, 22, 24 | — | recovery: limits 0–1000, vin/temp defaults |

`vlim`, `tlim`, `led`, `lederr` are **not wrapped** by the library (constants only) → send via `servoBus.write()/read()` with the byte layouts above (uint16 little-endian for the mV pairs).

### 6.4 Link a discovered ID to pan/tilt (persisted) — `go=` group **M**

Mirrors the existing `N`-setter pattern in `handleAction`:

- `M,PI,<id>` → `setPanId(id)`
- `M,TI,<id>` → `setTiltId(id)`

Each validates 0–253, persists its NVS key, and live-rebinds the backend (`backend->bind(PAN/TILT, id)`) — no reboot. Because these go through `/action`, the reply is the extended status CSV, so the new `panid`/`tiltid` fields echo back for confirmation.

> **UI wiring note.** Populate the ID choice from the scan results. Since `data-set` matches `<input>` only, either use `<input type=number data-set="M,PI">` (and drop the dropdown), **or** add an explicit `change` handler for a `<select>` that calls `send('M,PI,'+sel.value)`. Do not put `data-set` on a `<select>` and expect it to fire.

### 6.5 Live telemetry

Served **cached** in the extended `/status` CSV (fields 17–22, [§7](#7-protocol--endpoint-additions)); the background telemetry task ([§9.2](#92-telemetry-task)) does the real reads on its own cadence, so the existing 700 ms poll pays nothing. UI renders pan/tilt voltage, temperature, actual position, and a fault flag; shown only when `bknd=1`.

---

## 7. Protocol / endpoint additions

**The status CSV is preserved and only appended to.** `applyState` guards `p.length < 13` (`web_page.h` L262, `<` not `!=`) and reads only `p[0..12]`, so appended fields are backward-compatible.

### 7.1 Extended `fullStatus()` fields (append after field 13 `homing`)

| # | Field | Source | Phase |
|---|---|---|---|
| 14 | `bknd` (0/1) | RAM | 2 |
| 15 | `panid` | RAM | 2 |
| 16 | `tiltid` | RAM | 2 |
| 17 | `panVin` mV (−1 n/a) | cached telemetry | 3 |
| 18 | `panTemp` °C (−1) | cached | 3 |
| 19 | `tiltVin` mV | cached | 3 |
| 20 | `tiltTemp` °C | cached | 3 |
| 21 | `panActual` deg (−1) | cached (`readAngle`) | 3 |
| 22 | `tiltActual` deg | cached | 3 |
| 23 | `fault` (pan\|tilt bitmask, −1 n/a) | firmware-derived ([§9.3](#93-derived-fault-state)) | 3 |

> **Correction vs the initial draft (phasing).** Identity fields **14–16 land in Phase 2**, not Phase 3 — otherwise the Phase-2 `M,PI`/`M,TI` setters can't be verified via the CSV reply. Telemetry fields 17–23 remain Phase 3.

> **Correction vs the initial draft.** `s.reserve(64)` (L171) is now too small — a populated 23-field CSV measures ~96 chars, forcing a reallocation. **Bump to `s.reserve(112)`.**

### 7.2 `go=` additions (in `handleAction`, new `else if (group == "M")` after L445)

`M,PI,<id>`, `M,TI,<id>` — live ID link. Unknown groups already no-op and return state, so this is safe.

### 7.3 New dedicated endpoints (registered in `setup()` near L835–841, all `HTTP_GET` + CORS header)

| Endpoint | Purpose | Reply | Reboot? |
|---|---|---|---|
| `/servoscan` | bus discovery ([§6.1](#61-bus-scan--id-discovery--get-servoscan)) | TSV | no |
| `/servoid` | safe ID programming ([§6.2](#62-program-a-servos-id-safely)) | `ok\t…` / `fail\t…` | no |
| `/servocfg` | per-servo config read/write ([§6.3](#63-readwrite-all-config-per-servo)) | `key=value` / `ok` | no |
| `/setbackend?b=<0\|1>` | persist `bknd`, then `ESP.restart()` | `saved` | **yes** |

`/setbackend` mirrors `/setwifi`'s save-and-restart (L505–512): switching backends changes pin/peripheral usage, so a clean re-init is safest. Plain-text TSV / `key=value` throughout (no JSON lib — matches the existing minimalist parser).

---

## 8. Persistence (NVS namespace `"pantilt"`)

New keys (all ≤15 chars), added to `loadSettings()` after L204 with validation, each with a `setX` setter mirroring `setStep` (L273) that persists only its own key.

| Key | Var | Default | Validation |
|---|---|---|---|
| `bknd` | `bknd` | 0 (PWM) | `constrain(v, 0, 1)` |
| `panid` | `panid` | 1 | `constrain(v, 0, 253)` |
| `tiltid` | `tiltid` | 2 | `constrain(v, 0, 253)`; if `== panid`, keep but raise a UI collision **warning** (don't silently auto-fix) |

- **Do NOT cache full servo config in NVS.** Each LX-16A stores its own config in EEPROM and is the source of truth; caching would drift. Persist only the *identity/link* (backend + two IDs).
- **Reset semantics:** keep `bknd`/`panid`/`tiltid` **out of `C,RS`** (`resetCalibration`, L294–303) — they are hardware identity like `wssid`/`wpass`, not calibration. They get their own setter paths.
- **Deferred (v1 keeps as compile-time consts):** optional future keys `lxpin` (web-tunable bus pin) and `lxmove` (move-time tuning).

---

## 9. OLED + WS2812B integration

### 9.1 What is already backend-agnostic (no change)

- `signalMove()` / `updateLeds()` (L685–737) are driven by `panPos`/`tiltPos` deltas → keep working with either backend.
- `drawMotionScreen()` (L741) is driven by `ledAnim` + `panPos`/`tiltPos` → unchanged.
- `oledBar()`'s `/180` math → unchanged ([§5.3](#53-angle-mapping-0180-ui--lx-16a-0240--01000-ticks)).

### 9.2 Telemetry task

New `updateTelemetry()` called from `loop()` (near L850):

```text
if (bknd != LX16A) return;
if (millis() < ledActiveUntil) return;      // skip blocking reads during/just-after motion (NOT just homing)
if (millis() - telemLastMs < TELEM_MS) return;   // TELEM_MS ~1000
telemLastMs = millis();

// Priority: actual position each tick (the headline commanded-vs-actual feature).
read panActual (gate isCommandOk); read tiltActual (gate isCommandOk);
// Round-robin ONE of {panVin, panTemp, tiltVin, tiltTemp} per tick.
// Update the consecutive-failure counters used by the derived fault (§9.3).
```

> **Correction vs the initial draft.** Gating only on `homing` still hitches manual press-and-hold nudges (auto-repeat `REPEAT_MS=120`, which is *not* `homing`), and it pauses "actual position" during a home sweep — exactly when you'd want to watch it — while a pure 6-step round-robin refreshes each value only every `6 × TELEM_MS ≈ 6 s` (stale). Fix: gate on **recent motion generally** (`millis() < ledActiveUntil`, the signal `signalMove` already sets, L689), and **prioritize the position read every tick** so commanded-vs-actual is meaningful; round-robin only the slower voltage/temperature. Reads only ever run while idle, so the ~90 ms worst-case per idle tick (2 positions + 1 vin/temp, once/sec) never lands during a glide.

### 9.3 Derived fault state

> **Correction vs the initial draft (and both reviews' top finding).** There is **no live-fault register**; cmd 36 returns the *alarm-enable config*, so reading it and latching `LED_FAULT` on `fault != 0` would pin the LED red permanently (default mask 7). Compute the fault bitmask in firmware from cached telemetry:

```text
fault bit 0 (temp)  = readTempC ≥ configured/conservative temp cap (~70 °C)
fault bit 1 (volt)  = readVinMv outside [6000, 8400] mV
fault bit 2 (stall) = |commanded - readAngle| > STALL_DEG while torque enabled
fault "bus dead"    = N consecutive read failures (3–5) → treat as fault + offline hint
```

Cmd 35/36 stays in the config UI (`lederr`) only.

### 9.4 OLED — add a 4th "Servo Bus" page (only when `bknd=1`)

> **Correction vs the initial draft.** `drawOLED()` uses a **bare `else`** for the Calibration page (L639) — "page 2 *or anything else*." Simply bumping the page count would render Calibration on both page 2 and page 3, and the new page would never show. **First change L639 `} else {` → `} else if (oledPage == 2) {`, then add `else if (oledPage == 3) {`.**

- Page-count dynamic: `oledPageCount = bknd ? 4 : 3`.
- `oledPage = (oledPage + 1) % oledPageCount;` (L860, was `% 3`).
- `oledDots()` loop `i < 3` → `i < oledPageCount` (L590; it already positions dots by index).
- New page 3 "Servo Bus": backend name + `id pan/tilt`, `Pan 7.4V 42C`, `Tilt 7.4V 41C`, fault flag.
- **Position page (page 1):** when `bknd=1`, render commanded-vs-actual (`panActual`/`tiltActual`) — the genuinely new capability the serial backend enables.

### 9.5 WS2812B — add a fault state

Add `LED_FAULT` to `enum LedAnim` (L133), a red case in `animColor()` (L668), and a branch in `updateLeds` (L701) that **latches red when `fault != 0`** or telemetry reads persistently fail, clearing when it recovers — the recommended override of the idle mood light. Requires the consecutive-failure counter ([§9.3](#93-derived-fault-state)) so it can't strobe on a single failed read.

---

## 10. Safety

| Area | Measure |
|---|---|
| **ID collisions** | Program IDs one-at-a-time; `/servoid` broadcast path refuses unless `mode=lone` proves a single servo; targeted path pre-checks the new ID isn't already in use; verify-after-write; UI "single servo connected" checkbox + `confirm()`. |
| **Broadcast caution** | Broadcast used only for `mode=lone` discovery and (guarded) single-servo ID-write. **Never** broadcast a config/ID write to a populated bus. |
| **Over-voltage / over-temp** | `begin()` sets conservative windows (vin 6000–8400 mV, temp cap ~70 °C via cmd 22/24); the servo auto-cuts torque on breach; the **firmware-derived** fault surfaces on OLED page 3 + `LED_FAULT` + CSV field 23. **Never exceed 8.4 V.** |
| **Bus contention** | Single-threaded `loop()` serializes all bus access; `retry=0`; reads throttled, non-overlapping, and never during motion; 1 kΩ series resistor caps contention current. |
| **Un-brickable writes** | Validate every write server-side (ID 0–253; angle limits 0–1000 with min<max spanning the working range; vin 4500–12000; temp 50–100; trim ±125; lederr 0–7). `/servocfg?...set=factory` restores limits/vin/temp defaults. ID is always recoverable via `mode=lone` + re-program. |
| **No hard-hang** | **Never call `LX16AServo::calibrate()`** (`while(1)` + unbounded loop); use `setLimitsTicks()`. Prime servos in `begin()` so lazy init never blocks the loop. |
| **Mechanical** | `begin()` reads current position, then glides home over ~800 ms before enabling continuous control (torque-on won't snap a back-driven servo). `confirm()` before torque-unload while mounted (a loaded rig could drop). |
| **Best-effort reads** | Never trust a read without `isCommandOk()`; never declare "bus dead" on one failure (require N in a row). |

---

## 11. Migration & coexistence (SG90 ↔ LX-16A)

- **Both backends stay compiled in.** `bknd` selects at boot; default 0 = PWM, so existing SG90 hardware behaves **identically** until the user switches. PWM uses timers + pins 25/26; LX-16A uses UART on 17 — no conflict, only one active per boot.
- **Toggle** via `/setbackend?b=1` (persist + reboot). PWM telemetry returns `-1` → UI hides telemetry, OLED stays 3 pages.

**Migration runbook:**
1. **Bench-provision IDs one at a time** (pan=1, tilt=2) using `/servoid` (flow A) or the library's `lx16aSetServoID` example — one servo on the bus at a time.
2. Wire the shared bus (1 kΩ + 4.7 kΩ) + separate 6–8.4 V supply + common ground.
3. `/setbackend?b=1` → reboot.
4. Link IDs: `M,PI,1` / `M,TI,2`.
5. Recalibrate limits/home with the **existing** UI (logical 0–180, unchanged).
6. Set vin/temp limits via `/servocfg`.

---

## 12. Phased implementation plan

Each phase is a small, independently testable checkpoint.

### Phase 0 — Bench validation (**go/no-go gate**)
- Pin toolchain per decision D1 (`espressif32@3.5.0` for one-pin, or plan the buffer path); add `madhephaestus/lx16a-servo`.
- Bench-test the one-pin example on GPIO17 with **one** servo, 1 kΩ series + 4.7 kΩ pull-up, `retry = 0`.
- **Verify the load-bearing library API** against the installed source: `beginOnePinMode`, `write(cmd, params, len, id)` signature, the `retry` member, `isCommandOk()`, `id_verify()`, register constants, and especially **`pos_read()` units** ([§5.5](#55-reads-are-best-effort-critical)).
- **Measure read reliability:** log `pos_read`/`vin`/`temp` success rate over a few hundred reads. **If poor → switch to the buffered two-pin path ([§4.5](#45-buffer-mode-fallback-74hc126)) before proceeding.**
- **Test:** one servo moves, reports plausible telemetry, and read success rate is acceptable. Pre-assign pan=1, tilt=2 one at a time.

### Phase 1 — Abstraction (no behavior change)
- Add `servo_backend.h` (`enum Axis`, `ServoBackend`, `PwmBackend`); delete duplicate `Axis` at L82.
- Route the **6 runtime writes** through `backend->writeAngle()` and the setup block through `backend->begin()` (owning L807–818 incl. the home assignment). Still PWM.
- **Test:** operation is byte-for-byte identical to today — the safe checkpoint.

### Phase 2 — `Lx16aBackend` + backend switch
- Implement `writeAngle` (delta-scaled `lxMove`), `begin` (bus open, `retry=0`, prime, `setLimitsTicks(0,1000)`, protection windows, gentle home, torque), `readAngle/Vin/Temp`, `bind`.
- Add `bknd`/`panid`/`tiltid` NVS + setters + CSV **fields 14–16**; add `/setbackend` (endpoint **and** a UI toggle button) + `M,PI`/`M,TI`.
- **Test:** boot into LX-16A; nudge, home glide, soft limits, invert, LEDs, OLED all work; `M,PI`/`M,TI` echo the new IDs in the CSV.

### Phase 3 — Telemetry, OLED page, fault LED
- `updateTelemetry()` (motion-gated, position-priority); CSV **fields 17–23**; firmware-derived fault; UI telemetry display; OLED 4th page (with the L639 `else if` fix) + commanded-vs-actual on page 1; `LED_FAULT` with the N-failure counter.
- **Test:** live V/temp/actual on page + OLED; fault flashes red only after a real breach / N read failures, and clears on recovery.

### Phase 4 — Config web surface
- `/servoscan` (range + lone), `/servocfg` read + per-field validated write + `factory`. Bespoke JS handlers modeled on the WiFi block (dropdown population, per-field forms) — **its own work item**, not `data-set` reuse.
- UI cards: angle/vin/temp limits, mode, torque, trim+save, LED, LED-error.
- **Test:** read/change each register and confirm the change on the physical servo.

### Phase 5 — Safe ID programming
- `/servoid` flows A (broadcast, `mode=lone`-gated) and B (targeted, dup-checked) + UI (one-at-a-time, `confirm()`, verify-after-write).
- **Test:** re-ID a lone servo and read it back; confirm the >1-servo refusal and the duplicate pre-check both trigger.

### Phase 6 — Hardening + docs
- Protection defaults, `factory` recovery, tune `LX_MS_*` and `TELEM_MS`, update `PROJECT.md` / the design spec under `docs/superpowers/specs/`.

---

## 13. Open decisions & risks

### 13.1 Decisions for the human

| # | Decision | Recommendation |
|---|---|---|
| D1 | **Toolchain:** pin `espressif32@3.5.0` (core 1.0.6) for one-pin mode *and regress the whole existing build*, **or** adopt the 74HC126 buffer path (core-agnostic, 3 GPIOs + WS2812B move)? | Try one-pin first; let Phase 0 read-reliability decide. Biggest decision. |
| D2 | **Board module:** confirm it is **not** an ESP32-WROVER (GPIO16/17 = PSRAM). | Already-driven GPIO16 implies fine — confirm. |
| D3 | **Power:** source (2S LiPo vs bench PSU), current rating (stall ~1–2 A/servo), fuse value. | 2S LiPo or 7.4 V PSU; ~3–5 A fuse. |
| D4 | **Angle mapping:** 1:1 physical (0–180 of the 240° range) vs full-range stretch; keep 0–180 vs widen to 240 later. | 1:1 physical, keep 0–180 (both reviews concur). |
| D5 | **Backend switch:** reboot-to-apply vs live re-init. | Reboot (mirrors WiFi; safest re-init). |
| D6 | **`LX_MS_*` tuning:** floor/ceil/`k` for the delta-scaled move time. | Start floor 40 ms, ceil 220 ms, k 6 ms/deg. |
| D7 | **ID defaults** pan=1/tilt=2 and collision handling. | Warn on `panid == tiltid`, don't auto-fix. |
| D-verify | **`pos_read()` units** (centidegrees vs ticks). | Confirm on bench in Phase 0; use the matching `readAngle` conversion. |

### 13.2 Risks / library quirks (designed around)

| Risk | Mitigation |
|---|---|
| One-pin **writes** always report success (no ack) — can't detect a dead bus from writes | Detect liveness via periodic reads + `LED_FAULT` on N consecutive read failures |
| One-pin **reads** intermittently fail/misparse (echo loopback) | `isCommandOk()` gate; `retry=0`; N-failure threshold; **Phase 0 go/no-go** may push to the buffer path |
| `pos_read()` returns cached-on-failure | Always gate on `isCommandOk()` |
| `move_time()` / `readLimits()` / `setLimitsTicks()` spam `Serial` | Raw `lxMove` on the hot path; limit/read calls confined to `begin()`/config |
| Lazy `initialize()` blocks ~120–240 ms on first read | Prime each servo in `begin()` before the loop |
| Bricking via bad angle/vin limits | Server-side validation + `factory` recovery + `setLimitsTicks(0,1000)` in `begin()` |
| Shared-bus ID collision (two un-provisioned ID-1 servos) | Mandatory bench provisioning + `mode=lone` gate on `/servoid` |
| Blocking reads jank HTTP/OLED during motion | Reads only while idle (`millis() < ledActiveUntil`); lengthen `TELEM_MS` if glide still stutters |
| `calibrate()` hard-hang | Never call it |

---

## 14. Files touched

| File | Changes |
|---|---|
| `platformio.ini` | Pin core (D1); add `madhephaestus/lx16a-servo` to `lib_deps`. |
| `esp32_pan_tilt/servo_backend.h` | **New.** `enum Axis`, `ServoBackend` (incl. `bind`), `PwmBackend`, `Lx16aBackend`. |
| `esp32_pan_tilt/esp32_pan_tilt.ino` | Move `Axis` to header (delete L82 dup); 6 `writeAngle` sites + `begin()` block; NVS keys + setters; `group M`; new endpoints; `updateTelemetry()`; derived fault; OLED 4th page (+ L639 `else if` fix, dynamic page count); `LED_FAULT`; extended `fullStatus()` (+ `reserve(112)`). |
| `esp32_pan_tilt/web_page.h` | Advanced-panel servo cards (telemetry, ID link, config, scan, ID-program) modeled on the WiFi block; new DOM refs; `applyState` fields 14–23; **bespoke** `fetch` handlers for the four new endpoints (not `data-set`/`applyState`). |

---

## 15. References

| # | Source | Use |
|---|---|---|
| R1 | **madhephaestus/lx16a-servo** — `https://github.com/madhephaestus/lx16a-servo` | Chosen Arduino/ESP32 library (one-pin + buffer modes; `LX16ABus` / `LX16AServo`). |
| R2 | **maximkulkin/lewansoul-lx16a** — `https://github.com/maximkulkin/lewansoul-lx16a` | Python reference implementation; protocol/units cross-check. |
| R3 | **SpaceMaster85/lewansoul_lx16a** — `https://github.com/SpaceMaster85/lewansoul_lx16a` *(handle per research; confirm exact repo)* | C++/Linux reference. **Do not** copy `enableAll/disableAll` (they send cmd 13, not cmd 31). |
| R4 | **LewanSoul / HiWonder "LX-16A Bus Servo Communication Protocol"** (datasheet PDF) | Packet framing, command numbers, unit/voltage/baud facts ([§2](#2-lx-16a-background)). |

---

## Appendix A — Review corrections incorporated

Every valid finding from the two adversarial reviews is folded into the sections above; summarized here for traceability.

| Severity | Finding | Where fixed |
|---|---|---|
| HIGH | Toolchain: `platform@2.0.0` conflates platform vs core; correct pin is `@3.5.0` (core 1.0.6) + full regression, or the buffer path | [§3.2](#32-toolchain--platform-vs-core), D1 |
| HIGH | "Open-drain, no resistor" is backwards — it's push-pull half-duplex; use the author's **1 kΩ** series + 4.7 kΩ pull-up (not 100–220 Ω) | [§2.3](#23-half-duplex-on-the-esp32--how-the-library-actually-does-it), [§4.3](#43-signal-line-protection) |
| HIGH | One-pin reads are intrinsically best-effort (echo loopback); tolerate failure, N-fail threshold, Phase 0 go/no-go | [§5.5](#55-reads-are-best-effort-critical), [§9.3](#93-derived-fault-state), Phase 0 |
| HIGH | Fault read via cmd 36 is alarm-**config**, not live status → LED stuck red; derive fault in firmware | [§2.4](#24-command-set-used-by-this-project), [§9.3](#93-derived-fault-state) |
| HIGH | OLED bare `else` (L639) swallows the new page 3 | [§9.4](#94-oled--add-a-4th-servo-bus-page-only-when-bknd1) |
| HIGH | ID "single servo" range-sweep (hi=30) misses IDs 31–253 → use `mode=lone`; `id_write` is broadcast-only; targeted path needs a dup pre-check and raw write; scope the >1-refusal to the broadcast path | [§6.2](#62-program-a-servos-id-safely) |
| HIGH | GPIO16/WS2812B vs UART2 default RX — verified safe in **one-pin** mode (explicit pins); a real conflict in the **buffer** path | [§4.2](#42-lx-16a-bus-pin-gpio-17), [§4.5](#45-buffer-mode-fallback-74hc126) |
| MED | Lazy `initialize()` blocks ~120–240 ms on first read; prime in `begin()`; never call `calibrate()` | [§5.6](#56-begin--priming-and-hardware-limits), [§3.1](#31-library-quirks-to-design-around-verified-against-source) |
| MED | Raw `lxMove` bypasses the servo's EEPROM angle clamp → `setLimitsTicks(0,1000)` in `begin()` | [§5.6](#56-begin--priming-and-hardware-limits) |
| MED | 74HC126 fallback pins (RX17/TX18/flag19) don't work against the stock library; it forces RX16/TX17 → move WS2812B, then RX16/TX17/flag18 | [§4.5](#45-buffer-mode-fallback-74hc126) |
| MED | New web UI is a substantial bespoke JS surface (WiFi-block pattern), not `data-set` reuse; `data-set` matches `<input>` only | [§6](#6-web-feature-set), [§6.4](#64-link-a-discovered-id-to-pantilt-persisted--go-group-m), [§14](#14-files-touched) |
| MED | Telemetry gated only on `homing` hitches manual nudges and staleness; gate on `ledActiveUntil`, prioritize position | [§9.2](#92-telemetry-task) |
| MED | Single fixed `LX_MOVE_MS` too abrupt for nudges/reclamp; scale by commanded delta | [§5.4](#54-hot-path-write--bypass-the-wrapper), D6 |
| MED | Read/write unit asymmetry (ticks vs centidegrees) unverified; make symmetric, verify Phase 0 | [§5.5](#55-reads-are-best-effort-critical), D-verify |
| MED | `bind()` missing from base interface → compile error in PWM mode | [§5.1](#51-the-seam) |
| LOW | Write cost understated (~1 ms → ~3–4 ms/servo) | [§5.6](#56-begin--priming-and-hardware-limits) |
| LOW | `s.reserve(64)` too small for 23 fields → 112 | [§7.1](#71-extended-fullstatus-fields-append-after-field-13-homing) |
| LOW | Write-site double-count → 6 `writeAngle` + `begin()` owning L807–818 (incl. home assignment) | [§5.2](#52-the-two-backends-and-the-write-site-accounting) |
| LOW | Identity CSV fields belong in Phase 2 (so M-setters verify); place the `/setbackend` UI toggle | [§7.1](#71-extended-fullstatus-fields-append-after-field-13-homing), Phase 2 |
| LOW | Library API is unverified until installed — make Phase 0 confirm each call | Phase 0 |
