# ESP32 Pan / Tilt Servo Controller

A plain **ESP32** (no camera) drives two hobby servos — **pan** and **tilt** — from a
self-contained **web page**, a physical **I²C joystick**, or the **HTTP API**. Servos run on a
pluggable backend (**SG90 PWM** or an **LX-16A serial bus**). It adds a **128×128 OLED** cockpit,
**4 WS2812B** motion-indicator LEDs, per-axis **soft limits**, a calibrated **home** with an
adjustable **glide speed**, **WiFi provisioning** from the browser, and **persistent settings**
saved to flash.

> Derived from the [ESP32_CAM_pan_tilt](https://github.com/ErroujiOussama/ESP32_CAM_pan_tilt)
> project — camera removed, control layer rebuilt and extended.

**📖 [Read the Owner's Manual](https://siujacky.github.io/esp32-pan-tilt/manual.html)** — wiring,
web UI, the OLED cockpit with screen-by-screen figures, smart-servo features, HTTP API, and
troubleshooting. (Also downloadable from [Releases](../../releases), source at `docs/manual.html`.)

---

## Contents
1. [Features](#1-features)
2. [Parts](#2-parts)
3. [Wiring](#3-wiring)
4. [Repository layout](#4-repository-layout)
5. [Software setup](#5-software-setup)
6. [Configuration](#6-configuration)
7. [Build & flash](#7-build--flash)
8. [Usage](#8-usage)
9. [HTTP API](#9-http-api-reference)
10. [Tunable constants](#10-tunable-constants)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. Features

- **2-servo pan/tilt** — pan on **GPIO 25**, tilt on **GPIO 26** (50 Hz, full 0–180° travel).
- **Pluggable servo backend** — SG90 **PWM** (default) or an **LX-16A serial bus** (GPIO 17); a
  control-target router drives PWM, LX, or **both**, each target keeping its own position.
- **Physical joystick "cockpit"** — a nulllab mini-joystick (I²C) navigates an on-OLED cockpit:
  **DRIVE** flies the rig (RATE jog or ABSOLUTE glide), **MENU** configures it (buttons C/D/A/B).
- **Self-contained web UI** — a 2-tab page (PWM · LX-16A) with a D-pad and press-and-hold
  auto-repeat; served entirely from the ESP32 (no internet needed), so it works in hotspot mode.
  The advanced settings are grouped into **config tabs** (Rig · Servos · Joystick · System) so a
  setting is one tap away instead of a long scroll; the Servos tab appears only on the LX backend.
- **HOME glide** — a center/home button that **smoothly glides** both servos back at an
  adjustable speed (10–300 °/s) instead of snapping.
- **Speed / step** — a slider sets degrees-per-nudge (1–30°).
- **Soft limits** — per-axis min/max travel windows so a servo can't drive past a stop.
- **Home calibration** — "home" is a position you set (capture the current angle, or type it).
- **Persistent settings** — limits, home, home speed and step size are saved to **NVS flash**
  and restored (validated) on every boot, kept **separately for each control target** so the PWM
  and LX-16A rigs can have different travel windows, homes and speeds.
- **WiFi: Station with Access-Point fallback** — joins your network; if it can't, it hosts its
  own hotspot so it's never unreachable.
- **WiFi provisioning** — scan for networks and join one **from the web page** (no re-flashing).
- **128×128 SH1107 OLED** — an interactive **cockpit**: a live DRIVE HUD and joystick-navigated
  MENU pages (Config / Position / Calibration / Connection / Servo Bus).
- **4× WS2812B motion LEDs** — a slow rainbow **mood light** when idle, and a **per-direction
  color animation** while moving so onlookers can see motion at a glance.
- **Status LED + on-page badge** — the onboard LED and a page badge show the WiFi mode.

**Not included (intentionally):** camera/video, position presets/sequences, motion recording,
OTA updates, authentication.

---

## 2. Parts

| Part | Qty | Notes |
|---|---|---|
| ESP32 dev board | 1 | Plain ESP32 (e.g. "ESP32 DevKit v1", 30/38-pin). **No camera.** |
| SG90 micro servo | 2 | Pan + tilt (a standard pan/tilt bracket kit works). |
| LX-16A serial servo | 2 | **Optional** alternative to SG90 — serial "smart" servos sharing one bus (GPIO 17). |
| SH1107 **128×128** I²C OLED | 1 | Optional. The controller runs fine without it. |
| nulllab mini-joystick | 1 | **Optional** — I²C physical control (the "cockpit"); shares the OLED bus. |
| WS2812B RGB LED | 4 | Optional. Corner "motion" indicator. |
| External 5 V power supply | 1 | Powers the servos (and LEDs). **Not optional** — see the warning below. |
| Jumper wires | some | Signals + common ground. |
| USB cable (data) | 1 | Flashing + serial. |

---

## 3. Wiring

| Function | ESP32 pin | Connect to |
|---|---|---|
| **Pan** servo signal (PWM) | **GPIO 25** | Pan SG90 signal (orange/yellow); pin is web-settable (Digital tab) |
| **Tilt** servo signal (PWM) | **GPIO 26** | Tilt SG90 signal (orange/yellow); pin is web-settable (Digital tab) |
| **LX-16A serial bus** | **GPIO 32** | LX-16A signal, one-pin half-duplex (**LX backend only**; pin is web-settable) |
| **OLED + joystick SDA** | **GPIO 21** | OLED SDA **and** joystick SDA (shared bus) |
| **OLED + joystick SCL** | **GPIO 22** | OLED SCL **and** joystick SCL (shared bus) |
| **WS2812B data** | **GPIO 16** | First pixel `DIN` |
| Status LED | GPIO 2 (onboard) | — (built in) |

**Angle model:** pan `0 = left · 90 = center · 180 = right`; tilt `0 = down · 90 = center · 180 = up`.

**Shared I²C bus:** the OLED (**0x3C**) and the nulllab mini-joystick (**0x5A**) both sit on GPIO
21/22 — wire both to the same SDA/SCL. Only one servo backend is active at a time (PWM *or* LX-16A).

**WS2812B corner order** (as wired, `DIN` → out): `LED1 = top-right`, `LED2 = top-left`,
`LED3 = bottom-left`, `LED4 = bottom-right`.

**OLED / LED power:** OLED `VCC` → 3V3, `GND` → GND. WS2812B `5V` → external **5 V**, `GND` →
common ground, `DIN` → GPIO 16.

> [!CAUTION]
> **POWER THE SERVOS (AND LEDS) FROM AN EXTERNAL 5 V SUPPLY — NOT THE ESP32's 3V3 PIN.**
>
> Two SG90s can spike **> 500 mA**. Drawing that through the ESP32's 3V3 rail will **brown out
> and reset the board**.
> - Servo/LED **V+ → external 5 V supply**, never the ESP32 3V3 pin.
> - **COMMON GROUND IS MANDATORY:** tie the supply ground to the ESP32 **GND**. Without a shared
>   ground the signals have no reference and the servos jitter / the LEDs glitch.
> - Only the **signal/data** wires go to the ESP32 GPIOs.

> [!NOTE]
> WS2812B expects ~5 V logic on `DIN`; the ESP32 drives 3.3 V. For a handful of LEDs this usually
> works. If the first pixel shows a wrong/flickery color, add a 3.3→5 V level shifter on `DIN` (or
> a "sacrificial" first pixel used only as a level buffer).

---

## 4. Repository layout

```
esp32_pan_tilt/
  esp32_pan_tilt.ino   # firmware (motion, web, OLED cockpit, joystick)
  servo_backend.h      # ServoBackend seam: PwmBackend / Lx16aBackend + LX-16A helpers
  web_page.h           # INDEX_HTML: the self-contained control page (PROGMEM)
joystick_handle/       # vendor nulllab mini-joystick library (reference only - not compiled)
platformio.ini         # PlatformIO build (src_dir = esp32_pan_tilt)
README.md              # this file
lessonlearn.md         # engineering lessons + gotchas
PROJECT.md             # full technical documentation
serial-servo.md                  # LX-16A serial-servo design notes
serial_upgrade_and_joystick.md   # dual-backend + cockpit plan, with build progress
docs/superpowers/specs # the design specs (v1 + v2)
```

The firmware includes `web_page.h` from the same folder. Open the **`esp32_pan_tilt`** folder in
the Arduino IDE, or build the whole repo with PlatformIO.

---

## 5. Software setup

**Libraries** (all install from the respective package managers):

| Library | Source | Purpose |
|---|---|---|
| `ESP32Servo` | madhephaestus/ESP32Servo | servo PWM (SG90 backend) |
| `lx16a-servo` | madhephaestus/lx16a-servo | LX-16A serial backend |
| `U8g2` | olikraus/U8g2 | SH1107 OLED |
| `Adafruit NeoPixel` | adafruit/Adafruit NeoPixel | WS2812B LEDs |
| `WiFi`, `WebServer`, `Wire`, `Preferences` | bundled with ESP32 core | networking, I²C, NVS |

> The nulllab mini-joystick needs **no library** — it is read directly over `Wire` (see `PROJECT.md`
> §9a).

**Arduino IDE:** install the **ESP32 board package** (Boards Manager URL
`https://espressif.github.io/arduino-esp32/package_esp32_index.json`), then the three libraries
above from **Manage Libraries**. Open `esp32_pan_tilt/esp32_pan_tilt.ino`. (The nulllab joystick
needs no library — it is read directly over `Wire`.)

**PlatformIO:** nothing to install manually — `platformio.ini` already lists the dependencies:

```ini
[platformio]
src_dir = esp32_pan_tilt

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    madhephaestus/ESP32Servo
    madhephaestus/lx16a-servo
    olikraus/U8g2
    adafruit/Adafruit NeoPixel
```

---

## 6. Configuration

Most things are set **from the web page** and persist. Only a few compile-time constants live at
the top of `esp32_pan_tilt.ino`:

```cpp
const char* HOME_SSID = "YOUR_WIFI_SSID";     // fallback creds if none provisioned via web
const char* HOME_PASS = "YOUR_WIFI_PASSWORD";
const char* AP_SSID   = "ESP32-PanTilt";      // hotspot name
const char* AP_PASS   = "pantilt123";         // hotspot password (>= 8 chars, WPA2)
```

> [!IMPORTANT]
> You do **not** have to hardcode your WiFi — provision it from the web page (§8). If you do edit
> `HOME_SSID`/`HOME_PASS`, **never commit real credentials**. Keep the placeholders in any shared
> copy. The ESP32 radio is **2.4 GHz only**.

---

## 7. Build & flash

**PlatformIO (CLI):**
```
pio run                              # compile
pio run -t upload --upload-port COMx # flash (COMx = your board's port)
pio device monitor                   # serial @ 115200
```

**Arduino IDE:** Board = **"ESP32 Dev Module"**, select the port, **Upload**. If upload stalls at
"Connecting…", hold the **BOOT** button. First-time driver issues → install **CP210x** or **CH340**.

Typical footprint: **Flash ≈ 82 %** of the default partition, **RAM ≈ 15 %**.

---

## 8. Usage

### WiFi
On boot the ESP32 tries **Station** mode (LED fast-blink). Credentials come from NVS (if you
provisioned any) or the compiled placeholders.
- **Connected →** stays STA at your router-assigned IP (shown on Serial + the OLED). LED **solid**.
- **Timed out (~15 s) →** starts the **`ESP32-PanTilt`** hotspot at **192.168.4.1**. LED **slow-blink**.

| LED | Mode |
|---|---|
| Solid | Station (joined your WiFi) |
| Slow blink | Access Point (hotspot) |
| Fast blink | Connecting (boot) |

### Provision WiFi from the browser
Open the page → **tap the badge** (`AP · 192.168.4.1 ⚙`) to reveal advanced settings → **WiFi
setup** → **Scan networks** → pick your SSID → password → **Save & join**. It saves to NVS and
reboots to join; the **new IP appears on the OLED**. **Forget — hotspot only** clears it.

### Control page
- **D-pad** `▲ ◀ ⌂ ▶ ▼` — arrows nudge; **hold to auto-repeat**. `⌂` (center) = **HOME glide**.
- **Speed / Step** slider — degrees per nudge (1–30°).
- **Advanced** (tap the badge): **Soft limits** (numeric fields or "Set = current" capture),
  **Home** (Set Home = current / numeric / **home-speed** slider), **Reset calibration**.

### Joystick cockpit (optional)
A nulllab **mini-joystick** on the I²C bus drives the OLED as a two-mode **cockpit** — no phone
needed. The stick's centre-click (OK) is unused; actions are on the ring buttons:

| Button | Action |
|---|---|
| **D** | toggle **DRIVE ⇄ MENU** (anytime) |
| **C** | confirm / edit (in MENU) |
| **A** | home (in DRIVE) |
| **B** | back / cancel edit |

- **DRIVE** — the stick flies pan/tilt. **RATE** = hold to jog (faster the further you push);
  **ABSOLUTE** = the stick position maps to an angle and the rig glides there. Auto-returns to MENU
  after 5 s idle.
- **MENU** — stick **U/D** moves the selection, **L/R** changes page. On **Config**, press **C** to
  edit **Speed / Joy mode / Step**, **U/D** to change, **C** to confirm.

**Remapping the buttons:** open the web page → tap the badge → **Joystick** card. You can also
enable/disable the stick and switch RATE/ABSOLUTE there. Remapping is web-only on purpose — the
stick can't rebind itself, so a bad map can never lock you out of the menu that fixes it.

### OLED (optional)
The OLED is the cockpit display (above). In **MENU** the stick pages through:

| Page | Shows |
|---|---|
| **Config** (editable) | Speed, Joy mode (RATE/ABSOLUTE), Step |
| **Position** | Pan/Tilt angles + bars marking the soft-limit window and home; "HOMING" while gliding. On the LX-16A backend these are the servos' **own reported angles** ("reported" tag; `N/a` for a servo that doesn't answer); PWM shows the commanded angle (no feedback hardware) |
| **Calibration** | step, home speed, pan/tilt limits, home, uptime |
| **Connection** | WiFi mode, SSID, IP (AP password in hotspot mode) |
| **Servo Bus** (LX-16A only) | backend, IDs, per-servo V/°C, actual angle, fault; **Confirm = rescan the bus** (found IDs listed) |
| **Servo** (LX-16A only, editable) | the FULL per-servo setting set, scrollable: axis→**Bus ID link**, angle limits, **trim + EEPROM save**, voltage limits, temp cap, torque, LED, alarm mask, servo/motor **mode + speed** — parity with the SpaceMaster85 / lewansoul-lx16a config tools. (Reprogramming a servo's own ID stays web-only — it needs the confirm+verify flow) |

In **DRIVE** it shows a live HUD (big Pan/Tilt + bars). Bottom dots track the MENU page.

### WS2812B LEDs (optional)
- **Idle:** a slow rainbow **mood light** across the 4 corners at ~30 %.
- **Moving:** a **per-direction** animation —

| Move | LEDs |
|---|---|
| Pan right | **blue** dot, clockwise |
| Pan left | **magenta** dot, anti-clockwise |
| Tilt up | **green** bar, rising |
| Tilt down | **amber** bar, falling |

---

## 9. HTTP API reference

All `/action` and `/status` responses are `text/plain` with header
`Access-Control-Allow-Origin: *`.

| Request | Action |
|---|---|
| `GET /` | serve the control page |
| `GET /action?go=B,L` / `B,R` | pan − / + step (clamped to pan limits) |
| `GET /action?go=B,U` / `B,D` | tilt + / − step (clamped to tilt limits) |
| `GET /action?go=B,H` | start the smooth HOME glide |
| `GET /action?go=S,<n>` | set step size (1–30) |
| `GET /action?go=V,<n>` | set home glide speed (10–300 °/s) |
| `GET /action?go=C,SH` | set home = current position |
| `GET /action?go=C,PL` / `C,PH` / `C,TL` / `C,TH` | capture a limit = current |
| `GET /action?go=C,RS` | reset calibration to defaults |
| `GET /action?go=N,PL,<v>` / `N,PH,<v>` / `N,TL,<v>` / `N,TH,<v>` | set a limit = v |
| `GET /action?go=N,HP,<v>` / `N,HT,<v>` | set home pan / tilt = v |
| `GET /action?go=M,PI,<id>` / `M,TI,<id>` | set LX-16A pan / tilt servo bus ID (0–253, live rebind) |
| `GET /action?go=T,<0\|1\|2>` | control target: 0 = PWM, 1 = LX-16A, 2 = both |
| `GET /action?go=J,<0\|1>` | enable / disable the joystick |
| `GET /action?go=Y,<0\|1>` | joystick drive mode: 0 = RATE (hold to jog), 1 = ABSOLUTE (glide to stick) |
| `GET /action?go=R,<0\|1>` | LX idle torque: 0 = hold position, 1 = release ~3 s after each move settles (no buzz; no holding force) |
| `GET /action?go=K,<HM\|BK\|EN\|TG>,<0-4>` | remap a cockpit button (home / back / confirm / toggle) to A·B·C·D·OK |
| `GET /status` | current state |
| `GET /scan` | list visible WiFi networks (`ssid⇥rssi⇥locked` per line) |
| `GET /setwifi?ssid=..&pass=..` | save WiFi creds to NVS and reboot to join |
| `GET /forgetwifi` | clear saved creds and reboot |
| `GET /servoscan` | scan the LX-16A bus and list responding servo IDs |
| `GET /servoid?…` | program a single LX-16A servo's ID (one servo on the bus at a time) |
| `GET /servocfg?…` | read / write an LX-16A servo's limits + torque |
| `GET /setbackend?…` | switch the servo backend (PWM ⇄ LX-16A); persists and reboots |
| `GET /setlxpin?pin=<n>` | set the LX-16A bus GPIO; validates, persists and reboots (`fail⇥reason` if unusable) |
| `GET /setpwmpins?pan=<n>&tilt=<n>` | set the PWM servo GPIOs; validates the pair (collisions included), persists and reboots |
| `GET /lxprobe[?hi=N]` | read-only bus scan (default IDs 1–12): ID, voltage, temp, position per servo; works in both backends, no reboot, nothing moves |
| `GET /lxpintest` | electrical check on the bus pin (driven high / tied to ground / free); fully re-arms the bus after |
| `GET /lxtx` | wire-level transmit proof: the bus RX hears our own TX echo (plus any servo reply) |
| `GET /lxwiggle` | broadcast (ID 254) torque-on + small sweep clamped inside soft limits — moves any servo regardless of ID |
| `GET /lxfix` | force a full half-duplex bus re-arm + clamped broadcast sweep (recovery) |
| `GET /lxcal[?reset=1]` | stiction auto-trim: learned per-axis/per-direction overshoots + the last 10 settle records |

**`/action` and `/status` both reply with the same 31-field CSV** (so the page has one parser):

```
0-12  mode,ip,pan,tilt,step,panMin,panMax,tiltMin,tiltMax,homePan,homeTilt,homeSpeed,homing
13-15 bknd,panid,tiltid
16-22 panVin,panTemp,tiltVin,tiltTemp,panActual,tiltActual,fault   (LX telemetry; -1 = n/a in PWM)
23-26 ctrlTarget,joystickPresent,joystickEnabled,joyMode
27-30 mapHome,mapBack,mapConfirm,mapToggle                         (button remap; 0-4 = A/B/C/D/OK)
31    lxpin   32 lxRelax   33-34 pwmPanPin,pwmTiltPin
32    lxRelax                                                      (idle torque release 0/1)
```

Fields 2–11 are the **active target's** config — switching tab (`T,…`) re-populates them. All
positions (0–180°), step (1–30) and speed (10–300) are **clamped server-side**. Fields are
**append-only**, so a client that reads only the first 13 keeps working.

---

## 10. Tunable constants

At the top of `esp32_pan_tilt.ino`:

| Constant | Default | Purpose |
|---|---|---|
| `PIN_PAN` / `PIN_TILT` | 25 / 26 | servo signal pins |
| `PIN_OLED_SDA` / `PIN_OLED_SCL` | 21 / 22 | OLED I²C (auto-tries the swapped order too) |
| `PIN_LEDS` / `NUM_LEDS` | 16 / 4 | WS2812B data pin + count |
| `INVERT_PAN` / `INVERT_TILT` | false | flip a servo that moves the wrong way |
| `SERVO_MIN_US` / `SERVO_MAX_US` | 500 / 2500 | pulse range (full SG90 travel) |
| `HOME_DEG` | 90 | first-boot / reset home angle |
| `WIFI_STA_TIMEOUT_MS` | 15000 | how long to try WiFi before AP fallback |
| `STATUS_LED_PIN` | 2 | onboard status LED (`-1` disables) |
| `MOOD_VAL` | 76 | idle LED brightness (~30 %) |
| `LED_HOLD_MS` / `LED_FRAME_MS` | 300 / 80 | motion animation timing |
| `JOY_MS` | 30 | joystick poll interval (ms) |
| `JOY_SLOW_MS` / `JOY_FAST_MS` | 220 / 40 | RATE-mode jog interval at min / max deflection |

The OLED driver is `U8G2_SH1107_PIMORONI_128X128_F_HW_I2C` — see §11 if yours renders shifted.

---

## 11. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| A servo moves the **wrong way** | orientation | set `INVERT_PAN` / `INVERT_TILT = true`, re-flash |
| Servos **jitter / board resets** | 3V3-powered servos or no common ground | external 5 V + **common ground** (§3) |
| **OLED dark** | not detected on the bus | check wiring/power; the firmware auto-tries both SDA/SCL orders and logs `OLED OK at 0x..` or `OLED not found` on Serial |
| **OLED image shifted / mirrored** | wrong SH1107 variant (column offset) | swap the `oled` type to `U8G2_SH1107_SEEED_128X128_F_HW_I2C` or plain `U8G2_SH1107_128X128_F_HW_I2C` |
| **No hotspot** | still within the 15 s STA attempt | wait for the timeout; LED slow-blink = AP up |
| **WiFi scan blips the hotspot** | single radio (scan needs the station on) | expected — the list still returns; connection resumes after ~3 s |
| **Settings lost after reboot** | — | they shouldn't be; they're in NVS namespace `pantilt`. `Forget`/`Reset` clear them |
| **LED wrong color / direction** | color or CW/CCW mapping | edit `animColor()` / swap `LED_PAN_CW`↔`LED_PAN_CCW` (no rewiring) |
| **First LED wrong/flickery** | 3.3 V data into WS2812B | add a level shifter or sacrificial first pixel (§3) |
| Serial shows servo `LEDC ... already attached` **[E]** | harmless ESP32Servo quirk | ignore — `Servos attached: pan=1 tilt=1` confirms they work |
| **Upload fails** | driver / cable / port | install CP210x/CH340, use a **data** cable, hold **BOOT** |
| **Joystick does nothing** | not detected, or disabled | Serial should print `Joystick: nulllab mini @ 0x5A`; enable it (web `J,1`). The stick-click (OK) is intentionally unused — use **C/D/A/B**. Confirm the stick is actually being moved (see `lessonlearn.md` §13) |
| **Joystick drives the wrong way** | axis orientation | set `INVERT_PAN`/`INVERT_TILT`, or flip the sign in `readJoystick()` |
| **Can't switch to LX-16A** | backend defaults to PWM | `GET /setbackend` (or the web LX tab) — it persists and reboots. LX reads are unvalidated on the modern core (PROJECT.md §5, decision D1) |
| **LX scan/telemetry empty** | bus engine not armed, wiring, or servo power | reads are fully supported (half-duplex engine, D1 resolved — `lessonlearn.md` §20–21). `GET /lxfix` re-arms the bus, `GET /lxtx` proves transmit at the wire level (RX hears our own echo), `GET /lxpintest` checks the line electrically |
| **LX servo totally dead — no motion, no telemetry** | firmware wasn't transmitting at all (historical), or wiring/power | the `pinMode`-detach bug is fixed in this firmware (D1). If it recurs: `GET /lxtx` (wire-level transmit proof) then `GET /lxwiggle` (ID-agnostic broadcast sweep, clamped to soft limits) |
| **Rig moves on its own / OLED menu changes by itself** | flaky joystick I2C connection | fixed: failed joystick reads are now inert (skipped) instead of decoding as full-stick; after 100 consecutive failures the stick is declared offline until reboot. Reseat the joystick's 4 wires |
| **LX servo does nothing at all** | wrong bus pin, or power | check the **LX-16A bus pin** on the web page matches your wiring; the servo needs its **own 6–8.4 V** supply with **common ground** — never the ESP32's 3V3/VIN |
| **LX servo powered from the ESP32 5V/VIN → never answers** | **5 V is below the LX-16A's 6 V minimum**, so its controller never boots | give it 6–8.4 V from its own supply. `GET /lxpintest` confirms it: an unpowered servo clamps the bus **LOW even against the internal pull-up**, a powered one idles it **HIGH**. Also a hazard — an LX-16A draws amps and will brown out the board |
| **`pio` upload fails with `UnicodeEncodeError: 'charmap'`** | Windows console is cp1252; esptool's progress bar is Unicode | `export PYTHONIOENCODING=utf-8` (or `chcp 65001`) before `pio run -t upload` |

See **`lessonlearn.md`** for the story behind several of these, and **`PROJECT.md`** for the full
technical design.
