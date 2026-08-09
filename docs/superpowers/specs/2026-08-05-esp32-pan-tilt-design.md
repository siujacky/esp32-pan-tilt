# ESP32 Pan/Tilt Servo Controller — Design Spec

- **Date:** 2026-08-05
- **Status:** Approved (brainstorming complete)
- **Derived from:** https://github.com/ErroujiOussama/ESP32_CAM_pan_tilt (camera removed, control layer rebuilt)

## 1. Overview

A plain **ESP32** (no camera) drives two hobby servos — **pan** and **tilt** — over a
phone/laptop-friendly web page. The page is a D-pad: arrow buttons nudge each servo, a
center **HOME** button re-centers both, and a **speed slider sets the step size**
(degrees moved per nudge). WiFi tries the user's network first and falls back to its own
hotspot if that fails.

### In scope
- 2-servo pan/tilt control on GPIO 25 (pan) and GPIO 26 (tilt)
- D-pad web UI with press-and-hold auto-repeat
- HOME button (re-center both servos)
- Speed control = step size per nudge (1–30°)
- WiFi Station-first, Access-Point fallback
- Onboard-LED status indication + on-page mode badge

### Out of scope (YAGNI)
Camera/video, position presets, motion sequences/recording, OTA updates, authentication,
persistent settings in flash.

## 2. Hardware & wiring

| Function | GPIO | Angle model |
|---|---|---|
| **Pan** (horizontal) | **25** | 0 = left · 90 = center · 180 = right |
| **Tilt** (vertical) | **26** | 0 = down · 90 = center · 180 = up |
| Status LED | 2 (onboard) | solid = STA · slow-blink = AP · fast-blink = connecting |

> ⚠️ **Power:** Two SG90s can spike >500 mA. Power servo **V+ from an external 5 V supply**,
> **not** the ESP32's 3V3 pin. **Tie the external supply ground to the ESP32 GND** (common
> ground) or the servos will jitter / brown-out the board. Servo signal wires → GPIO 25/26.

## 3. Configuration constants (top of the sketch)

```cpp
// WiFi — Station credentials are placeholders; real values are pasted in locally,
// NEVER committed to git.
const char*    HOME_SSID          = "YOUR_WIFI_SSID";
const char*    HOME_PASS          = "YOUR_WIFI_PASSWORD";
const char*    AP_SSID            = "ESP32-PanTilt";
const char*    AP_PASS            = "pantilt123";   // must be >= 8 chars (WPA2)
const uint32_t WIFI_STA_TIMEOUT_MS = 15000;

// Servos
const int  PIN_PAN      = 25;
const int  PIN_TILT     = 26;
const int  SERVO_MIN_US = 500;    // full SG90 travel (original used 1000–2000 = half range)
const int  SERVO_MAX_US = 2500;
const int  SERVO_MIN_DEG = 0;
const int  SERVO_MAX_DEG = 180;
const int  HOME_DEG      = 90;    // center
const bool INVERT_PAN    = false; // flip if a servo moves the wrong way
const bool INVERT_TILT   = false;

// Control
int        stepSize      = 5;     // degrees per nudge — the "speed" control
const int  STEP_MIN      = 1;
const int  STEP_MAX      = 30;

// Status LED (-1 disables)
const int  STATUS_LED_PIN = 2;
```

## 4. Servo motion model

State: `int panPos = HOME_DEG; int tiltPos = HOME_DEG;` plus `Servo panServo, tiltServo;`
and `enum Axis { PAN, TILT };`. Servos attached with `setPeriodHertz(50)` and
`attach(pin, SERVO_MIN_US, SERVO_MAX_US)`. Both written to `HOME_DEG` at startup.

### 4.1 `nudge(axis, dir)` — core motion function — **IMPLEMENTED BY THE USER**

Signature: `void nudge(Axis axis, int dir)` where `dir` is `+1` or `-1`.

Behavior contract (the user writes ~7 lines to satisfy this):
1. `delta = dir * stepSize`
2. If that axis's invert flag (`INVERT_PAN` / `INVERT_TILT`) is set, negate `delta`.
3. Add `delta` to that axis's stored position; **clamp** to `[SERVO_MIN_DEG, SERVO_MAX_DEG]`.
4. Write the clamped position to that axis's servo.

> This function ships as a **documented empty stub** that compiles (logs
> `"nudge() not implemented yet"` once). All call sites and surrounding scaffolding are
> complete. Agents must NOT implement or "fix" it — it is an intentional learning exercise.

### 4.2 `home()`
Set `panPos = tiltPos = HOME_DEG`, write both servos. (Implemented normally.)

## 5. WiFi state machine (boot-time decision)

```
setup()
 └─ WiFi.mode(WIFI_STA); WiFi.begin(HOME_SSID, HOME_PASS)
      loop until connected OR WIFI_STA_TIMEOUT_MS elapsed  (LED fast-blink)
      ├─ connected → apMode=false; WiFi.setAutoReconnect(true); ip=WiFi.localIP(); LED solid
      └─ timed out → WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID, AP_PASS);
                     apMode=true; ip=WiFi.softAPIP()  (=192.168.4.1); LED slow-blink
```
`loop()` runs `server.handleClient()` and, when `apMode`, a millis-based slow blink of the
status LED (non-blocking). Serial prints the final mode + IP at 115200 baud.

## 6. HTTP protocol (library: `WebServer.h`, port 80)

| Route | Method | Behavior | Response (text/plain) |
|---|---|---|---|
| `/` | GET | serve control page | `INDEX_HTML` via `server.send_P(200,"text/html",INDEX_HTML)` |
| `/action?go=<cmd>` | GET | run a command (below) | `"<pan>,<tilt>,<step>"` e.g. `95,90,5` |
| `/status` | GET | current mode + state | `"<STA|AP>,<ip>,<pan>,<tilt>,<step>"` |
| anything else / missing `go` | GET | not found | HTTP 404 |

All `/action` and `/status` responses set header `Access-Control-Allow-Origin: *`.
`go` is parsed with the original's `getValue(data, ',', idx)` comma splitter.

### Command → action mapping (`go=` value)

| `go=` | Action | Call |
|---|---|---|
| `B,L` | pan left  | `nudge(PAN, -1)` |
| `B,R` | pan right | `nudge(PAN, +1)` |
| `B,U` | tilt up   | `nudge(TILT, +1)` |
| `B,D` | tilt down | `nudge(TILT, -1)` |
| `B,H` | home      | `home()` |
| `S,<n>` | set step size | `stepSize = constrain(n, STEP_MIN, STEP_MAX)` |

Invert flags are applied **inside** `nudge()`, so the L/R/U/D→dir mapping is fixed and
direction reversal is a one-line config change. All positions and the step value are
clamped **server-side** regardless of client input.

## 7. Web UI (`web_page.h`)

`const char INDEX_HTML[] PROGMEM = R"rawliteral( ... )rawliteral";`

**Hard requirement: fully self-contained.** No external CSS/JS/fonts/CDN — when the ESP32
is in AP mode there is no internet, so any remote resource would fail. All styles and
scripts inline.

Layout (dark, mobile-first):
- Title `ESP32 Pan / Tilt` + a mode **badge** (`STA · 192.168.1.42` / `AP · 192.168.4.1`),
  filled from `/status` on load.
- D-pad cross: `▲` (up) top, `◀` `⌂` `▶` middle (HOME is the center ⌂), `▼` (down) bottom.
- Live readout: `Pan 90° · Tilt 90°`, updated from every `/action` response.
- **Speed / Step** slider `min=1 max=30 value=5` with a live `5°` label.

Interaction:
- Direction buttons use **Pointer Events** with **press-and-hold auto-repeat** (~120 ms):
  `pointerdown` sends once and starts an interval; `pointerup` / `pointerleave` /
  `pointercancel` **and** window `blur` clear it (no stuck key). `touch-action:none` +
  `preventDefault` so holding doesn't scroll/zoom.
- HOME sends `B,H` once (no repeat).
- Slider updates its label on `input`, sends `S,<n>` on `change`.
- All requests via `fetch()`; on network error show a small "disconnected" state; parse the
  `pan,tilt,step` reply to refresh the readout.

## 8. File structure

```
esp32_pan_tilt/
  esp32_pan_tilt.ino   # includes <WiFi.h> <WebServer.h> <ESP32Servo.h> "web_page.h"
  web_page.h           # INDEX_HTML (PROGMEM, self-contained)
README.md              # parts, wiring + power warning, IDE/board/lib setup, flashing, usage
```

## 9. Verification checklist

Firmware can't be unit-tested; verification is static review + a manual run:
- Compiles under ESP32 Arduino core with `ESP32Servo` installed (`arduino-cli` if available).
- Page loads with **zero** external requests (works with no internet in AP mode).
- Web page `?go=` strings exactly match the firmware's parser and command table.
- `◀▶` drive the **pan** servo (GPIO 25); `▲▼` drive **tilt** (GPIO 26); directions sane.
- HOME re-centers both to 90°. Speed slider changes nudge distance. Readout tracks reality.
- `AP_PASS` length ≥ 8. Step clamped 1–30 and angle clamped 0–180 even against a hostile query.
- Auto-repeat never sticks (release / leave / blur all stop it).
- `nudge()` remains an untouched documented stub for the user.

## 10. Build workflow (ultracode)

1. **Build** — 3 parallel agents: firmware, `web_page.h`, README (fixed contract above).
2. **Review** — 4 adversarial lenses: protocol-consistency, ESP32/hardware, web-UI, security.
3. **Verify** — each finding independently confirmed real before it counts.
4. **Fix** — confirmed findings applied; `nudge()` stub preserved.
