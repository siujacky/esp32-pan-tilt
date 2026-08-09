# Plan v2: Independent Dual Backends + Joystick/OLED "Cockpit"

Status: **partially implemented** — see §0 Build progress. Supersedes v1 of this file. Builds on
`serial-servo.md` and the LX-16A code already in the firmware.

Locked decisions from review: **D-A = independent angles per backend.** D-B (speed) and D-C (joystick
control mode) are no longer static config — they become **live, on-device settings on an OLED config
page** driven by the joystick.

Target files: `esp32_pan_tilt/servo_backend.h`, new `esp32_pan_tilt/cockpit.h` (joystick + OLED UI),
`esp32_pan_tilt/esp32_pan_tilt.ino`, `esp32_pan_tilt/web_page.h`, `platformio.ini`.

---

## 0. Build progress (updated 2026-08-08)

What is **implemented** vs. still planned. In practice the cockpit was built **inline in
`esp32_pan_tilt.ino`**, not in a separate `cockpit.h`.

| Item | Status | Notes |
|---|---|---|
| Backend router + 2-tab web (P1) | ✅ done, flashed | `driveAngle()` fans to PWM / LX / Both via `ctrlTarget`; web has PWM · LX tabs |
| Independent **positions** per target (P1b) | ✅ done, flashed | via `ctrlSlot[3]` snapshot/restore on target switch |
| Independent **config** per target (§3.1) | ✅ done, flashed + verified | `CtrlState` now carries limits/home/speed/step too; per-target NVS keys (`pmin0`/`pmin1`/…) with migration from the legacy shared keys. Verified on hardware: migration preserved the saved limits/home, and a per-target write survived a reboot. **Deviation:** see "swap model" note below |
| Joystick reading (P3) | ✅ done, flashed | **nulllab mini @ 0x5A** (raw I²C, not seesaw): X=`0x10`, Y=`0x11`; buttons `0x20`–`0x24`. Confirmed sweeping 0–255 on hardware |
| Cockpit slice 1: DRIVE/MENU + Config page | ✅ done, flashed | `cockMode`, MENU nav (`stickStep`), Config edits Speed/JoyMode/Step; 5 s DRIVE idle → MENU (D-G) |
| Joystick modes RATE + ABSOLUTE (D-E) | ✅ done, flashed | RATE jog + ABSOLUTE glide; switchable on the Config page; `joyMode` persisted (`joymd`) |
| Multi-servo scan → dropdown → link (P2) | ✅ already done | web Bus-scan (range/lone) populates the pan/tilt ID dropdowns; per-servo config card edits one ID at a time. (Was mis-listed as pending.) |
| Per-servo OLED page (limits + torque, D-F) | ✅ done, flashed | MENU page 5 (LX only): pick PAN/TILT servo, edit angle-limit min/max (ticks) + torque; cached, throttled bus read; renders `--` when the bus doesn't answer. **Needs LX hardware** to exercise for real |
| Web button-remap UI (D-B, web-only) | ✅ done, flashed + verified | Joystick card: enable, drive mode, and 4 action→button selects (`K,<HM\|BK\|EN\|TG>,<0-4>`); duplicate warning; OLED hints follow the map. Verified: `K,EN,4` and `Y,1` round-tripped through a reboot, then restored to A/B/C/D + RATE |
| Verify joystick → servo drive on hardware | 🔶 **still unverified** | stick reads confirmed; the DRIVE→servo path has never been confirmed moving a servo. (Indirect evidence: the OLED Config editor demonstrably changed Speed and Step during earlier button testing, so stick→action works.) |
| **LX-16A hardware bring-up (D1)** | ✅ **fully resolved — motion AND reads verified** | Root cause was ours, not the wiring: `pinMode()` on arduino-esp32 3.x detaches the pad from the UART, so `lx16a-servo`'s direction-flip meant **nothing was ever transmitted**. Final fix: **GPIO-matrix half-duplex engine** (library bus objects deleted entirely) — RX permanently attached, TX matrix-routed per packet. Verified live: scan finds ID 1 with V/°C/pos, `/servocfg` full readback, telemetry streaming in `/status`, closed-loop motion (commanded 90 ↔ measured 88–89). See `lessonlearn.md` §20–21 |
| **Stiction auto-trim** (user's staircase idea) | ✅ built + hardware-verified | settle → measure shortfall → bump past target (≤2×) → learn overshoot per axis/direction → apply on first command next time. `GET /lxcal` shows learned trims + last 10 settle records. Verified: a (bogus) 4.0° trim self-corrected to convergence and first-shot landings hit within 1° |
| **Crash-loop audit (14-agent, 2026-08-09)** | ✅ fixed + hardware-verified | The interim TX-only fix left the library bus object un-initialized: 6 call paths (5 HTTP endpoints + OLED Servo page) were null-deref **crash-reboots**, reproduced + backtrace-decoded. Also fixed: joystick I2C failure decoding as full-stick **runaway** (now inert + offline latch), `/lxtx`//`lxpintest` silently killing motion, per-boot broadcast **EEPROM wear**, boot homing rigs to a **foreign target's calibration**, glide **snap** after blocking handlers, false-negative diagnostics, fault noise for a never-fitted axis |

> **Swap-model deviation from §3.1.** Rather than the full `AxisConfig`/`Controller` rewrite, each
> target's position **and** config live in `ctrlSlot[t]`; the live globals always belong to the
> active target (`saveSlot`/`loadSlot` on switch). This keeps every motion function untouched.
> The one behavioural difference: **only the active target glides** — an inactive target's home
> glide is *paused* and resumes when you switch back to it, instead of both gliding concurrently.

> **Critical bug caught in review, before it bit.** Extending `CtrlState` from 4 to 12 members
> turned a *pre-existing* short aggregate-init in `setup()` into a config-zeroing bug that wiped
> every target's calibration on boot (latent until the first target switch, then capable of
> commanding a servo to 0°). Fixed member-wise, and `-Wmissing-field-initializers` is now a build
> flag so the whole class of bug fails at compile time. Full write-up: `lessonlearn.md` §18.

See `lessonlearn.md` §12–15 for the joystick/cockpit engineering notes.

---

## 1. The vision — a phone-optional cockpit

The seesaw joystick + SH1107 OLED turn the rig into a **self-contained controller**. No web page
required for everyday use: you **fly** the gimbal with the stick and **configure** it through an
on-screen menu — all from the four buttons and the stick.

| Ask | How it lands |
|---|---|
| Independent angles per backend | Each backend keeps its **own** pan/tilt; DRIVE flies the active one; both glide independently. |
| OLED config page (switch **mode** = D-C, change **speed** = D-B) | A **Config** page in the MENU, edited with U/D + OK. |
| Joystick L/R = page, U/D = function | **MENU mode**: L/R cycles pages, U/D moves the item cursor, OK edits/acts. |
| "think out of the box" | **DRIVE / MENU** dual-mode joystick, a live HUD, headless operation, per-servo menu pages. |
| (retained from v1) 2-tab web, multi-servo scan/dropdown, per-servo detail | Web mirrors the on-device state; OLED pages are the same scanned servo table. |

---

## 2. Baseline (what exists)

Backend seam (`PwmBackend`/`Lx16aBackend`), a single `backend` chosen by NVS `bknd`; one shared 0–180
motion model (pos/limits/home/glide → 6 `writeAngle` sites); LX endpoints `/servoscan` `/servoid`
`/servocfg`; OLED auto-cycling pages + "Servo Bus"; WS2812B motion LEDs; I²C on 21/22.

Three seams change: **(a)** motion state becomes **per-backend**; **(b)** a **router** fans writes to
targeted backend(s); **(c)** a new **cockpit** input/UI layer (joystick + OLED menu).

---

## 3. Independent per-backend motion (D-A)

### 3.1 Each backend is a fully independent controller (positions AND config)

Locked D-A2: because the two backends "may control different things," they are **fully independent** —
each owns its position *and* its own limits / home / speed / step / invert.

```cpp
struct AxisConfig {                          // per-backend, persisted as one NVS blob
  int  panMin, panMax, tiltMin, tiltMax;     // its own soft-limit window
  int  homePan, homeTilt, homeSpeed, stepSize;
  bool invertPan, invertTilt;
};
struct Controller {                          // one complete controller per backend
  AxisConfig cfg;                            // independent config
  int  pan, tilt;                            // independent position (0..180)
  bool homing; uint32_t homeLastMs;          // independent glide
};
Controller ctrlPwm, ctrlLx;
Controller& active() { return (ctrlTarget == T_SERIAL) ? ctrlLx : ctrlPwm; }
```

- `nudge()`, `home()`, `updateHoming()` take a `Controller&` (+ its backend). **`updateHoming()` runs
  for BOTH** each loop → they glide independently (finish a PWM home while you jog LX).
- Clamp / home / invert / step all read the controller's **own** `cfg`, so the two backends can have
  completely different travel windows, home points and speeds — right for "they control different
  things."

### 3.2 The router

```cpp
enum Target { T_PWM=0, T_SERIAL=1, T_BOTH=2 };
int ctrlTarget = T_PWM;             // NVS "ctgt"; set by web tab or joystick
bool lxEnabled = false;             // NVS "lxen"; lazy lx.begin() on enable

AxisState& activeAx() { return (ctrlTarget==T_SERIAL) ? axLx : axPwm; }

void driveAxis(AxisState& ax, PwmOrLx which) { /* write ax.pan/ax.tilt to that backend */ }
// Every input (D-pad, joystick, web) moves activeAx() (or BOTH), then driveAxis() pushes to hardware.
```

`T_BOTH` sends the same nudge to **both** `AxisState`s (they move in parallel but remain separate
states, so they can be re-separated later). The 6 write-sites call the router.

### 3.3 Consequences of full independence (locked)
- **Two config sets in NVS** — stored as blobs `cfg0` (PWM) / `cfg1` (LX) via `prefs.putBytes`
  (avoids ~20 scalar keys), validated on load (clamp, min≤max, home-in-limits) like today.
- The web limits/home panels and the OLED Config page edit the **active** controller's `cfg`; switching
  target re-populates them. The status CSV's existing limit/home/pos fields therefore mean "the active
  controller's"; both backends' pan/tilt are **also** appended (§9) for at-a-glance display.
- `C,RS` resets the **active** controller's `cfg` only.

---

## 4. The Cockpit — joystick + OLED UI (the centerpiece)

One button toggles two interaction modes. The OLED is the screen for both.

```
        ┌─────────── DRIVE ───────────┐   MODE btn    ┌─────────── MENU ───────────┐
stick → │ L/R = pan, U/D = tilt        │  ◄────────►   │ L/R = page, U/D = item      │
        │ (active target, control law) │               │ OK = edit/act, Back = back  │
buttons │ OK=Home Back=Target Aux=SetH │               │ (values edited with U/D)    │
        │ OLED = live HUD              │               │ OLED = menu / detail        │
        └──────────────────────────────┘               └─────────────────────────────┘
```

### 4.1 Buttons are remappable (defaults shown; seesaw pins 3/13/2/14 → B1..B4)

Each physical button holds an **action** the user can reassign **from the web page only** (Serial tab;
remapping the buttons *with* the buttons is awkward, and it's a rare action). The OLED cockpit simply
*uses* whatever mapping is configured. The action decides what the button does in each mode. Defaults:

| Button | Action (default) | DRIVE effect | MENU effect |
|---|---|---|---|
| **B1** | `MODE` | switch to MENU | switch to DRIVE |
| **B2** | `OK` | HOME glide | enter/exit edit, or run an action item |
| **B3** | `BACK` | cycle target PWM→LX→Both | exit edit / back |
| **B4** | `SET_HOME` | Set Home = current | jump to the Config page |

**Assignable actions:** `MODE`, `OK`, `BACK`, `HOME`, `TARGET` (cycle), `SET_HOME`, `TORQUE` (toggle
active-LX torque), `STEP_UP`, `STEP_DN`, `NONE`. Stored as `btnAct[4]` (NVS blob `btns`). Validation
keeps the menu usable: **at least one button must map to each of `MODE`, `OK`, `BACK`** — the remap UI
refuses a save that drops one, and "reset to defaults" is always offered. Edge-detected + debounced;
`INPUT_PULLUP` (pressed = 0).

### 4.2 MENU pages (L/R cycles; U/D moves the cursor)

```
[0] Status      WiFi mode · IP · SSID · target · joystick mode          (read-only)
[1] Drive       big Pan/Tilt of active target (+ actual if LX)          (B1/OK → fly it)
[2] Config      the editable settings — see §5                          (U/D + OK edit)
[3] Servo bus   N servos · target · fault                               (if lxEnabled)
[4..] Servo #id per-detected-servo detail + editable limits/mode/torque (if lxEnabled)
[last] Calibrate joystick re-center + deadzone                          (action)
```

Page count is dynamic: `4 + (lxEnabled ? srvCount : 0) + 1`. Page dots become `"3/9"` text when many.

### 4.3 DRIVE HUD

Big live pan/tilt for the active target, a **reticle** showing stick deflection, the target badge
(PWM/LX/BOTH), and the joystick mode (RATE/ABS). Feels like a gimbal viewfinder. The WS2812B direction
animation still fires (via `signalMove`), so lights + HUD agree.

### 4.4 No-joystick fallback (unchanged behaviour)
If no seesaw is detected, the OLED keeps **auto-cycling** its pages exactly as today and the web is the
only controller. The cockpit layer is purely additive.

### 4.5 Safety / idle
- Entering DRIVE never moves a servo until the stick leaves the deadzone.
- **Idle timeout (D-G = 5 s):** after 5 s of no input in DRIVE, auto-return to MENU/Status (prevents
  forgotten drift). Editing times out back to view.
- Mode changes and edits are debounced; a value edit commits on OK (or reverts on Back).

---

## 5. The OLED Config page (D-B speed + D-C mode, on-device)

MENU page [2]. U/D selects an item; **OK toggles edit**, then U/D changes the value, OK/Back commits.
Items act on the **active target's** controller (each backend has its own `cfg`), so switching Target
re-populates them. Each mirrors a web setting and persists.

| Item | Type | Range | Backing |
|---|---|---|---|
| **Target** | enum | PWM · LX · Both | `ctrlTarget` (`ctgt`) |
| **Speed** *(D-B)* | int | 1–30 ° / nudge | `stepSize` (`step`) |
| **Joy mode** *(D-C)* | enum | **Rate** · **Absolute** | `joyMode` (`jmod`) — see §6 |
| **Home speed** | int | 10–300 °/s | `homeSpeed` (`hspd`) |
| **Invert pan / tilt** | bool | off/on | (new NVS `ipan`/`itil`, promoted from compile-time consts) |
| **Set home = current** | action | — | captures active target's pos → home |
| **LX bus** | enum | off · on | `lxEnabled` (`lxen`), lazy `lx.begin()` on enable |

*(Button remapping is **not** here — it lives on the web page only; §4.1.)*

So "switch mode and change speed on the OLED" is literally two items here — no phone.

---

## 6. Joystick control-law modes (D-C, live-switchable)

Two switchable modes — **velocity** (rate jog) and **position** (glide-to-aim). Set on the Config page;
both drive the active target and reuse `nudge()` / the glide engine → limits/invert/LEDs.

- **RATE — proportional-rate jog (default):** deflection past a deadzone → nudges at a rate ∝ magnitude
  (`interval = map(|defl|, dead, 511, 220ms, 40ms)`). Push further = pan faster; the number moved per
  tick is `stepSize`. Velocity control.
- **ABSOLUTE — glide-to-position (locked D-E):** stick position maps straight to a target angle within
  limits (`target = map(defl, -511, +511, min, max)`); the axis **glides** to it at `homeSpeed` and
  **never snaps**. Point the stick, the servo smoothly goes there. Position / "aim" control.

`joyMode` is one byte in NVS; switching is instant.

### 6.1 Reading the stick (non-blocking, `cockpit.h`)
Every `JOY_MS` (~30 ms): `analogRead(1)`→X, `analogRead(15)`→Y (0–1023), minus the boot-calibrated
center, deadzone ~80. In **MENU** the same deflections become **discrete nav events** (edge on
threshold + auto-repeat); in **DRIVE** they become continuous motion per the law above. Buttons via
`digitalReadBulk(mask)`.

---

## 7. Multi-servo bus (retained from v1)

- `/servoscan` fills a bounded cached table `SrvRow srv[LX_MAX_SERVOS=8]` (id + telemetry); telemetry
  task round-robins reads over **all** rows.
- **Web dropdown** (`<select>` of every found servo) → pick → `/servocfg` read/edit → configure one by
  one; link any two IDs to pan/tilt via `M,PI` / `M,TI`.
- **OLED** renders the same table as the per-servo MENU pages [4..] (§4.2, §5 per-servo edit of
  limits/mode/torque with U/D+OK).
- Safe ID programming rules (serial-servo.md §6.2: `mode=lone`, duplicate pre-check, `confirm()`)
  unchanged and now more important with N servos.

---

## 8. Web UI — the 2 tabs (retained, now mirrors the cockpit)

Tab bar **[ PWM ] [ Serial ] (+ mirror)** sets `ctrlTarget` live; the shared D-pad drives the selected
target. **Serial** tab: LX-enable, scan+dropdown config, pan/tilt link, telemetry, and a **joystick
panel** (present/enabled, mode Rate/Abs, calibrate, **button remap** B1–B4). Everything on the OLED Config page is also here —
the two UIs are two faces of the same state (same NVS, same `go=` setters), so changing speed on the
stick updates the web on its next poll and vice-versa.

---

## 9. Protocol / NVS

**`go=` setters** (all echo the status CSV): `T,<0|1|2>` target · `J,<0|1>` joystick enable ·
`Y,<0|1>` joyMode · `IP,<0|1>`/`IT,<0|1>` invert pan/tilt. **Endpoints:** `/servoenable?on=` (lazy LX
begin) · `/servolist` (cached table TSV). **CSV append** (after field 23): `ctrlTarget`, `lxEnabled`,
`joyPresent`, `joyMode`, `pwmPan`,`pwmTilt`,`lxPan`,`lxTilt` (both backends' positions). Fields 3/4
remain the active target's pos.

**NVS** (namespace `pantilt`): two per-backend **config blobs** `cfg0`(PWM)/`cfg1`(LX) via
`putBytes(AxisConfig)`; a **button-map blob** `btns` (`btnAct[4]`); plus scalars `lxen`, `ctgt`,
`joyen`, `jmod`, `panid`, `tiltid`. The old per-axis scalar keys (`pmin`…`step`…`hspd`, `ipan`/`itil`)
fold into `cfg0`/`cfg1`. Migrate once: seed both blobs from the current scalar config; `bknd==1 →
lxen=1, ctgt=1`. Identity / mode / button scalars stay out of `C,RS` (which resets only the active
controller's blob).

---

## 10. Hardware & wiring (delta from serial-servo.md)

```
I²C (GPIO 21 SDA / 22 SCL, 3V3):  ─ OLED 0x3C ─ Seesaw joystick 0x49   ← joystick adds NO new pins
PWM servos GPIO 25/26 ← 5 V   ·   LX bus GPIO 17(+1kΩ) ← 6–8.4 V(sep)   ·   WS2812B 16 ← 5 V
```
Joystick is STEMMA-QT on the same 4 I²C wires as the OLED. Both servo sets present at once (that's
"connect both to test"). Grounds common. Verify seesaw is the **FeatherWing** (`getVersion>>16 == 5753`,
addr 0x49, X=pin1/Y=pin15/btns 3,13,2,14); the Gamepad-QT variant differs — keep as constants.

---

## 11. Firmware structure

| File | Role |
|---|---|
| `servo_backend.h` | keep both backend classes; positions leave here (they live in `AxisState` in the `.ino`). |
| `cockpit.h` *(new)* | seesaw read + calibrate + button debounce; the DRIVE/MENU state machine; the menu model (pages × items with get/set); `drawCockpit()`; all no-ops when no joystick. |
| `.ino` | `axPwm`/`axLx` + router `driveAxis`; per-`AxisState` `nudge/home/updateHoming` (both glide); `ctrlTarget`/`lxEnabled`/`joyMode`/invert globals + NVS; new `go=`/endpoints; telemetry round-robin over `srv[]`; call `cockpit.tick()` in `loop()`; the OLED draw dispatches to the cockpit when a joystick is present, else the legacy auto-cycle. |
| `web_page.h` | 2 tabs; scan-dropdown config; joystick panel; render both backends' positions. |
| `platformio.ini` | add `adafruit/Adafruit Seesaw Library`. |

**A menu model** keeps it maintainable — an array of pages, each an array of items `{label, kind,
get(), set()/act()}`; navigation + rendering are generic, so adding a setting is one row, and the same
table feeds both the OLED and (optionally) the web.

---

## 12. Safety & edge cases

- Independent glides: both `AxisState`s can be homing at once; each `updateHoming` is guarded on its own
  `homing`. LED animation follows whichever moved last (`signalMove`), which is fine.
- DRIVE never moves until the stick leaves the deadzone; idle timeout returns to MENU (no forgotten
  drift). ABSOLUTE mode glides (never snaps) to the stick target.
- Shared I²C: OLED redraw + joystick read are sequential in `loop()`; LX telemetry is on UART (`Serial2`)
  — no contention with I²C. Skip the joystick read on a full-frame OLED redraw tick if latency shows.
- Graceful absence: no joystick → auto-cycle OLED + web only; LX empty/unplugged → `--`/`LED_FAULT`,
  never a hang (best-effort reads, `retry=0`).
- `T_BOTH` mirror clamps both to the same shared limits — one clamp protects both.
- **D1 (LX one-pin core)** still open (serial-servo.md); PWM default path unaffected throughout.

---

## 13. Phased implementation (each independently testable)

| Phase | Deliverable | Test |
|---|---|---|
| **P1** | Per-backend `AxisState` + router + `ctrlTarget`/`lxEnabled` + NVS migration; **2-tab web**. | PWM identical; tabs switch target; two independent positions visible in the web. |
| **P2** | Multi-servo scan → dropdown → per-ID config + link; cached `srv[]` + `/servolist`. | Dropdown lists all; config one by one; assign pan/tilt. |
| **P3** | `cockpit.h`: seesaw read + calibrate + buttons; **DRIVE mode** (RATE law) driving the active target; DRIVE HUD. | Stick jogs the active backend; buttons home/cycle-target; absent = graceful. |
| **P4** | **MENU mode**: L/R pages, U/D items, OK edit; **Config page** (speed=D-B, joy-mode=D-C, target, invert, set-home, LX enable); per-servo MENU pages. | Configure everything with the phone unplugged. |
| **P5** | **ABSOLUTE** joystick law; idle timeout / screensaver; polish. | Stick-to-angle aim; auto-return; edge cases. |

P1–P2 need no joystick and don't touch the PWM default. P3–P5 gate behind joystick auto-detect.

---

## 14. Decisions — all resolved

**D-A2** = fully independent per-backend config (§3.1/§3.3) · **D-B** = **remappable buttons**, web-only
(§4.1) · **D-E** = ABSOLUTE mode **glides** to the pointed angle (never snaps), switchable with the
**proportional-rate jog** mode (§6) · **D-F** = per-servo pages edit **limits + torque** (mode/LED
web-only) · **D-G** = DRIVE **idle-timeout 5 s**.

No open decisions remain — the plan is fully specified and ready to build.

---

## 15. References
`serial-servo.md` (LX-16A protocol/backend/safe-ID); Adafruit Seesaw `PC_Joystick.ino` (0x49,
X=1/Y=15, btns 3/13/2/14, ver 5753); `madhephaestus/lx16a-servo`; `adafruit/Adafruit Seesaw Library`;
`PROJECT.md`/`lessonlearn.md` (shared-Wire reuse gotcha).
