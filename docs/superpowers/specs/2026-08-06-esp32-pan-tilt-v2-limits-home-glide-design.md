# ESP32 Pan/Tilt v2 — Limits, Home Calibration, Home-Speed Glide, Persistence

- **Date:** 2026-08-06
- **Status:** Approved (brainstorming complete)
- **Builds on:** `2026-08-05-esp32-pan-tilt-design.md` (v1) — everything there still holds
  except where this doc overrides it. Also assumes the v1.1 OLED addition (SH1107 128×128,
  3 auto-cycling pages) already in `esp32_pan_tilt.ino`.

## 1. What v2 adds
1. **Soft limits** — settable min/max for pan and tilt; nudges and the home target clamp to them.
2. **Home calibration** — "home" is a stored position (`homePan`,`homeTilt`), not fixed 90°.
3. **Home-speed glide** — HOME performs a smooth, non-blocking glide at a settable deg/sec
   (fixes "home too fast"), instead of an instant `write`.
4. **Persistence** — limits, home, home-speed, and step size are saved to NVS and survive reboot.
5. **Both** calibration UIs — capture-current buttons AND numeric fields.

## 2. Data model (new/changed globals)

```cpp
#include <Preferences.h>

int panMin = 0,  panMax = 180;      // pan soft limits   (persisted)
int tiltMin = 0, tiltMax = 180;     // tilt soft limits  (persisted)
int homePan = 90, homeTilt = 90;    // calibrated home   (persisted)
int homeSpeed = 90;                 // home glide deg/sec (persisted)
const int HSPEED_MIN = 10, HSPEED_MAX = 300;
// stepSize (existing) is now also persisted.

bool     homing     = false;        // a home glide is in progress
uint32_t homeLastMs = 0;            // time base for the glide stepper
Preferences prefs;                  // NVS namespace "pantilt"
```

## 3. Persistence (NVS via Preferences)

- Namespace `"pantilt"`. Keys (≤15 chars): `pmin pmax tmin tmax hpan htilt hspd step`.
- `loadSettings()` — `prefs.begin("pantilt", false)` once in setup; read each with the defaults
  above; then **validate**: clamp each to 0..180; if `min>max` swap; `homePan=constrain(homePan,
  panMin,panMax)`; `homeTilt=constrain(homeTilt,tiltMin,tiltMax)`; `homeSpeed` to HSPEED range;
  `stepSize` to STEP range.
- Save the single changed key with `prefs.putInt(key, value)` immediately after every config
  change (limits/home/speed/step). NVS wear-levels; these writes are user-paced and rare.

## 4. Motion / glide engine

`panPos`,`tiltPos` remain the live angles. Two motion paths:

**Instant (nudges).** `nudge()` now (a) cancels any home glide (`homing=false`), (b) clamps to
the axis's soft limits instead of 0/180:
```
panPos = constrain(panPos + delta, panMin, panMax);   // (tilt: tiltMin/tiltMax)
```

**Glide (home).** `B,H` calls `startHome()` → `homing=true; homeLastMs=millis();`.
`updateHoming()` runs every loop pass while homing:
```
uint32_t now = millis();
int stepDeg = (int)((long)homeSpeed * (now - homeLastMs) / 1000);
if (stepDeg < 1) return;                 // let sub-degree time accumulate (don't advance base)
homeLastMs = now;
panPos  = stepToward(panPos,  homePan,  stepDeg);   // move by <=stepDeg toward target
tiltPos = stepToward(tiltPos, homeTilt, stepDeg);
panServo.write(panPos); tiltServo.write(tiltPos);
oledDirty = true;
if (panPos == homePan && tiltPos == homeTilt) homing = false;
```
`stepToward(cur,tgt,s)` = move `cur` toward `tgt` by at most `s` (min/max clamp at the target).
Both axes step at the same deg/sec, so the nearer axis simply arrives first. Non-blocking:
`updateHoming()` is called from `loop()` alongside `server.handleClient()` and the OLED — no `delay()`.

## 5. Calibration operations (clamping rules)

Capture-current and numeric setters share the same clamps; **every setter saves its NVS key**,
then re-clamps `panPos/tiltPos` into the (possibly new) limits and re-clamps `homePan/homeTilt`
into limits, writing servos if a live position moved:

| Operation | Effect |
|---|---|
| set home = current | `homePan=panPos; homeTilt=tiltPos` (each clamped to its limits) |
| set panMin (capture=current / numeric=v) | `panMin = constrain(value, 0, panMax)` |
| set panMax | `panMax = constrain(value, panMin, 180)` |
| set tiltMin | `tiltMin = constrain(value, 0, tiltMax)` |
| set tiltMax | `tiltMax = constrain(value, tiltMin, 180)` |
| set homePan (numeric) | `homePan = constrain(v, panMin, panMax)` |
| set homeTilt (numeric) | `homeTilt = constrain(v, tiltMin, tiltMax)` |
| set home speed | `homeSpeed = constrain(v, 10, 300)` |
| reset calibration | limits→0/180, home→90/90, homeSpeed→90; save all |

After any **limit** change: `panPos=constrain(panPos,panMin,panMax)` (+write if changed), same
for tilt; and re-clamp home into the new limits. This keeps everything mutually consistent.

## 6. Extended HTTP protocol

`getValue(go, ',', idx)` now uses up to index 2 (`N,<what>,<value>`). Full `go=` table:

| `go=` | Action |
|---|---|
| `B,L`/`B,R` | pan −/+ step (clamped panMin..panMax) |
| `B,U`/`B,D` | tilt +/− step (clamped tiltMin..tiltMax) |
| `B,H` | **start smooth home glide** to (homePan,homeTilt) at homeSpeed |
| `S,<n>` | stepSize = constrain(n,1,30); save |
| `V,<n>` | homeSpeed = constrain(n,10,300); save |
| `C,SH` | home = current position; save |
| `C,PL`/`C,PH` | panMin / panMax = current; save |
| `C,TL`/`C,TH` | tiltMin / tiltMax = current; save |
| `C,RS` | reset calibration to defaults; save |
| `N,PL,<v>`/`N,PH,<v>` | panMin / panMax = v; save |
| `N,TL,<v>`/`N,TH,<v>` | tiltMin / tiltMax = v; save |
| `N,HP,<v>`/`N,HT,<v>` | homePan / homeTilt = v; save |

**Unified reply.** Both `/action` and `/status` now return the SAME 13-field CSV
(`fullStatus()`), so the page has one parser:
```
mode,ip,pan,tilt,step,panMin,panMax,tiltMin,tiltMax,homePan,homeTilt,homeSpeed,homing
e.g.  AP,192.168.4.1,90,90,5,10,170,0,180,90,90,90,0
```
`homing` is `1`/`0`. Both responses keep the `Access-Control-Allow-Origin: *` header.
Missing `go` / unknown route → 404 (unchanged).

## 7. Web UI additions (`web_page.h`)

Keep the D-pad + step slider. The HOME (⌂) button now triggers the glide. Add:

- **Limits panel** — shows `Pan: min–max`, `Tilt: min–max`. For each of panMin/panMax/
  tiltMin/tiltMax: a **numeric input** (0–180) that sends `N,*,<v>` on `change`, AND a
  **"Set = current"** capture button that sends the matching `C,*`.
- **Home panel** — a **"Set Home = current"** button (`C,SH`); numeric inputs for homePan/homeTilt
  (`N,HP`/`N,HT`); a **Home-Speed** slider 10–300 deg/s (`V,<n>`) with a live label.
- **Reset** — a small "Reset calibration" button (`C,RS`) behind a `confirm()`.
- **`applyState(csv)`** — one function parsing the 13 fields; updates readout, badge, step
  slider, all limit/home numeric fields, speed slider, and shows a **"homing…"** indicator when
  field 12 is `1`. Used for BOTH `/action` replies and `/status`.
- **Periodic poll** — `setInterval(()=>fetch('/status').then(...applyState), 700)` so the page
  reflects the autonomous home glide and any other change. **Do not overwrite an input/slider
  that currently has focus** (skip focused elements in `applyState`) so polling never fights a
  user mid-edit.
- Still **fully self-contained** (no external CSS/JS/fonts/CDN). Keep the Pointer-Events
  press-and-hold auto-repeat with all its stop backstops, and the keyboard handlers.

## 8. OLED updates (3 pages stay; enrich pages 2 & 3)

- **Page 2 — Position:** each 0–180 bar additionally marks the **soft-limit window** (ticks at
  min & max, or brackets) and the **home** position (a small marker), plus the live fill. Show a
  small **"HOMING"** flag while `homing`.
- **Page 3 — Status/Calibration:** show `Step`, `Home spd`, `Pan lim min-max`, `Tilt lim min-max`,
  `Home pan/tilt`, and uptime — laid out to fit 128×128 (small font as needed).
- Page 1 (Connection) unchanged. Redraw is already `oledDirty`-driven + throttled; `updateHoming()`
  sets `oledDirty` so the glide animates on-screen.

## 9. Edge cases
- `min > max` can never persist: setters clamp so `min ≤ max` always.
- Home is always kept within the current limits (re-clamped whenever limits change).
- A nudge during a glide cancels the glide (manual override wins).
- `homeSpeed` clamped 10–300; a very low speed still moves (sub-degree time accumulates).
- Setting a limit that excludes the current position pulls the live position (and servo) inside.
- All numeric `go=` values are clamped server-side regardless of client input.

## 10. Verification additions
- Compiles with `Preferences.h` + `U8g2` + `ESP32Servo` (PlatformIO `esp32dev`).
- Reboot test: set a limit/home/speed, power-cycle → values restored from NVS.
- Glide test: HOME from a far position visibly sweeps at the set speed; raising the speed slider
  speeds it up; a D-pad press mid-glide stops it immediately.
- Limit test: nudge cannot pass min/max; tightening a limit past the current angle pulls it in.
- Web ↔ firmware: every `go=` in §6 is produced by the page and accepted by the firmware; the
  13-field reply parses correctly; a focused input is not clobbered by the 700 ms poll.
- `nudge()` unchanged in spirit (still clamp-not-wrap), just clamps to the soft limits now.
