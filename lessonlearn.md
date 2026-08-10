# Lessons Learned — ESP32 Pan/Tilt

A running log of the non-obvious problems this project hit and how each was diagnosed and fixed.
Every one cost real debugging time; they're written down so they don't cost it twice. Most were
found by **evidence** (serial logs, WiFi scans, reading library source, a compile) rather than
guessing.

---

## 1. On an ESP32-CAM, GPIO 25 & 26 belong to the camera

**Symptom / context.** The base project put the servos on GPIO 14/15 and used 25/26 for the
camera. The request was to put servos on **25 and 26**.

**Diagnosis.** On the AI-Thinker **ESP32-CAM**, GPIO 25 = camera `VSYNC` and GPIO 26 = camera
`SIOD` (I²C data). Worse, those pins aren't broken out to usable headers on that board — so the
fact that servos were physically wired to 25/26 proved the board was a **plain ESP32**, not a CAM.

**Fix / takeaway.** Confirm the board before choosing pins. A plain ESP32 makes 25/26 perfect
servo pins and lets the whole camera stack (and its port-81 stream server) be deleted. *A pin
number only means something relative to a specific board's pin-mux.*

---

## 2. I²C SDA/SCL are not interchangeable — and the ESP32 default is the opposite of many modules

**Symptom.** The OLED stayed dark. Serial: `OLED not found on SDA22/SCL21 (tried 0x3C/0x3D)`.

**Diagnosis.** The wiring was **SDA 21 / SCL 22** (the ESP32 default), but the firmware had been
told **SDA 22 / SCL 21** (from an earlier, mis-stated pin order). I²C is not symmetric — swap the
two lines and nothing ACKs.

**Fix.** Set `Wire.begin(SDA, SCL)` to match the wiring, **and** make `setupOLED()` auto-try the
swapped order as a fallback, logging which one worked. Now a mixed-up SDA/SCL self-heals.

**Takeaway.** For I²C, verify the pin order against the *board's* default, not the module's
silkscreen, and let firmware probe both orders when it's cheap to do so.

---

## 3. SH1107 128×128: the "very strange x offset" (register 0xD3)

**Symptom.** The OLED lit up but the whole image was **shifted ~32 px (¼ screen) to the right**.

**Diagnosis.** SH1107 128×128 panels differ in their display-offset register **0xD3**. U8g2's
*generic* `SH1107_128X128` driver comments out 0xD3 and instead bakes a **software `x_offset = 96`**
into its display info — correct for some panels, off by ~32 px for others. Reading the U8g2 source
(`u8x8_d_sh1107.c`) showed the **PIMORONI** and **SEEED** 128×128 variants use `x_offset = 0`. The
user independently confirmed via the esp-bsp driver that **0xD3 = 0x00** (offset 0) fixes this panel.

**Fix.** Use `U8G2_SH1107_PIMORONI_128X128_F_HW_I2C` (offset 0). One type name; no other change.

**Takeaway.** A constant pixel shift on an OLED = wrong controller **variant / column offset**, not
a wiring bug. The right fix is the matching driver, found by reading the driver's offset table —
not by nudging draw coordinates.

---

## 4. ESP32Servo logs a scary `[E]` error that is completely harmless

**Symptom.** Every boot:
```
[E] ledcAttachChannel(): Pin 25 is already attached to LEDC (channel 0, resolution 10)
[E] attachPin(): [ESP32PWM] ERROR PWM channel failed to configure on pin 25!
```

**Diagnosis.** Reading `ESP32PWM.cpp`: `attachPin(pin, freq, res)` calls `setup()` **and then**
`attachPin(pin)` — it configures each pin **twice** internally. The first succeeds; the second
logs "already attached." A library quirk, not our bug (it happens for everyone on ESP32 core 3.x).

**Fix.** Don't fight it — **prove** it's benign. A one-line probe after attach:
`Serial.printf("Servos attached: pan=%d tilt=%d\n", panServo.attached(), tiltServo.attached())`
prints `pan=1 tilt=1`, confirming the pins are correctly bound.

**Takeaway.** A red `[E]` from a library isn't automatically fatal. Turn "I think it's fine" into
"the firmware reports it's fine" with a targeted assertion.

---

## 5. A single-radio ESP32 can't be `WIFI_AP_STA` *and* a stable hotspot

**Symptom.** After adding WiFi provisioning, the `ESP32-PanTilt` hotspot **disappeared** — a PC
scan couldn't see it, though the serial log still printed `Started AP`.

**Diagnosis.** To let `/scan` work, the AP fallback had been switched to `WIFI_AP_STA`. But the
ESP32 has **one radio**: a station that keeps trying to (re)connect hops channels, which the softAP
can't tolerate — the AP stops broadcasting reliably.

**Fix.** Keep the fallback as **pure `WIFI_AP`** (rock-solid, one channel). Enable the station
**only for the ~3 seconds of an actual scan** inside `/scan`, then drop back to AP-only.

**Takeaway.** AP and STA share one radio and want opposite things (park vs. hop). Don't run both
continuously; borrow the station briefly and give it back. *This was a self-inflicted regression —
caught fast because a `netsh wlan show networks` scan gave a yes/no answer.*

---

## 6. Persisted state must never lag live state

**Symptom (caught in review, not in the field).** Tightening a soft limit pulled `home` inside the
new window **in RAM**, but the limit setter only saved its own NVS key — not the changed home.

**Diagnosis.** `reclampToLimits()` clamped `homePan/homeTilt` live but didn't persist them. Because
`loadSettings()` re-validates on boot, the simple case looked fine — which *masked* a real
inconsistency: shrink a limit (home moves, NVS stale), widen it back, reboot → home jumps to the
old value. For a feature whose headline is "settings survive reboot," that's a silent bug.

**Fix.** `reclampToLimits()` now writes `hpan`/`htilt` to NVS whenever the clamp actually moves them.

**Takeaway.** Whenever live state and stored state can diverge, they must be updated **together**.
A dedicated "persistence" review lens caught this where a generic pass didn't.

---

## 7. Smooth motion on a cooperative loop = time-based stepping with sub-degree accumulation

**Symptom (design).** "HOME is too fast." Servos were driven by instant `servo.write()`, so they
slewed at full hardware speed.

**Diagnosis / fix.** Added a **non-blocking glide**: store a target, and each `loop()` pass move
the current angle toward it by `homeSpeed × elapsed / 1000` degrees. The trap: at low speeds that
integer truncates to **0**, and if you still advance the time base the servo never moves. The fix
is to **return without advancing the base until at least 1° has accumulated**.

**Takeaway.** On a single cooperative loop (no threads, no `delay()`), animate by elapsed time and
carry the fractional remainder. Otherwise slow speeds silently stall.

---

## 8. The web page must be 100 % self-contained

**Symptom (would only appear in the field).** In hotspot mode there is **no internet**, so any
CDN font/script/stylesheet would fail and the UI would look broken.

**Fix / takeaway.** All HTML/CSS/JS is inlined into one `PROGMEM` string; even the favicon is a
`data:` URI. A review lens explicitly checks for **zero external resource references**. If a device
can run without a router, its UI must too.

---

## 9. One status contract beats many

**Design choice.** Both `/action` and `/status` return the **same CSV** (13 fields in v1; now **26**,
append-only). The page has a single `applyState()` parser used for command replies **and** the
background poll. Adding a field (e.g. `homing`, later the backend/joystick fields) means editing one
builder and one parser — and old clients that read only the first N keep working.

**Takeaway.** A single, versioned, **append-only** response shape removes a whole class of
client/server drift bugs — and it scaled cleanly from 13 fields to 26 as the firmware grew.

---

## 10. Provision-then-reboot is the robust way to switch networks

**Design choice.** Saving WiFi creds swaps the ESP32 from AP to STA. Doing that **live** means
migrating open sockets, DHCP and the very page you're standing on. Instead: write creds to NVS,
reply, and **reboot** — the existing STA-first→AP-fallback boot path does the rest. If the password
is wrong you're simply back on the hotspot; you can't get locked out. The OLED shows which way it went.

**Takeaway.** For mode-changing config, a clean reboot is often more reliable than a live switch —
especially when a persistence layer already makes boot deterministic.

---

## 11. WS2812B: 3.3 V data and idle current

**Notes.** WS2812B expect ~5 V logic on `DIN`; the ESP32 drives 3.3 V. A few pixels usually work,
but a wrong/flickery first pixel means you need a level shifter (or a sacrificial first pixel).
Brightness lives **in the color values** (not the global `setBrightness`) so the idle mood light
(~30 %) and the brighter motion colors can coexist; that also keeps continuous idle current low.

---

## 12. Reverse-engineering the nulllab mini-joystick (raw I²C at 0x5A)

**Symptom.** A physical joystick was wired in, but the firmware (written for an **Adafruit seesaw**
stick) never saw it. An I²C scan showed one unknown device at **0x5A**.

**Diagnosis.** It is a **nulllab mini-joystick module** — a raw I²C **register** device, *not* a
seesaw. A first probe read the wrong registers (0x00–0x09) with a **repeated-START**
(`endTransmission(false)`) and returned constant garbage. Reading the vendor's `JoystickHandle`
library settled both unknowns at once — the **register map** and the exact **read handshake**:

| Data | Register | Encoding |
|---|---|---|
| X axis | `0x10` | 0–255, centre 128 |
| Y axis | `0x11` | 0–255, centre 128 |
| Buttons OK / C / A / B / D | `0x20` / `0x21` / `0x22` / `0x23` / `0x24` | event: `0`=press-down, `3`=single-click, `6`=long-hold, `8`=idle |

The read is **write-register → full STOP → `requestFrom`** (`endTransmission()` with its default
`true`), never a repeated start. With the correct handshake the idle read is a clean `128 / 8`;
with the wrong one it is noise.

**Fix.** A tiny `miniRead(reg)` that mirrors the library's handshake exactly; buttons edge-detected
on the single-click event.

**Takeaway.** For an unknown I²C register device, read the vendor library for **two** things — the
register map **and** the stop/repeated-start handshake. A wrong handshake returns garbage even at
the right address, which is easy to misread as "wrong device."

---

## 13. The uncontrolled variable: confirm the stimulus before trusting the measurement

**Symptom.** Once the handshake was right, the joystick read perfect **idle** values
(`X=128 Y=128`, all buttons `8`) — but nothing changed across three separate captures. Only `RX`
(register 0x12) flickered `128↔145`. I concluded the live axis must be **0x12**, then — when even
holding the stick showed no range — began suspecting a **hardware fault**.

**Diagnosis.** Both conclusions were wrong, and for the same reason: **there was no confirmed
stimulus.** When finally asked directly, the user had **not been actuating the stick** during the
capture windows. A capture taken with confirmed live input immediately showed **LX (0x10) and LY
(0x11) sweeping the full 0–255** — the library's documented registers were correct all along, and
0x12's flicker was just idle noise on one channel. An intermediate "maybe I'm polling too fast and
starving the module's ADC loop" hypothesis was also cheaply **ruled out** (same frozen result at
the vendor's gentle 20 Hz cadence) — a distraction created by the same missing control.

**Fix.** Re-run the capture with the input confirmed applied; analyse **min/max per register**
instead of eyeballing a stream.

**Takeaway.** A measurement is meaningless without a confirmed stimulus. Before theorising from
data ("the axis is on 0x12", "it's a hardware fault"), verify the input is actually being applied —
**establish the control first.** On a hardware bench, the human is part of the test rig: state the
timing explicitly, or remove timing from the loop entirely (next lesson).

---

## 14. Verify with persistent state, not by racing a transient

**Symptom.** Confirming that the stick actually *drives the servos* kept failing: a 20–30 s
`/status` poll showed no movement, purely because live actuation and the capture window never
lined up.

**Fix / insight.** A joystick nudge writes `panPos`/`tiltPos` and they **persist** — the servo
stays where it was put. So the test needs no live timing: **move the stick, let go, then read
`/status`** — a resting position off-centre proves the whole chain (stick → `readJoystick` →
`nudge` → router → servo) end to end.

**Takeaway.** Design verifications around **durable state** you can read afterward, not transients
you must catch in the act. It removes the flakiest variable — synchronising the observer with the
event — from the test.

---

## 15. A thumbstick's centre-click is a poor "OK"

**Symptom.** The mini-joystick's **OK** is the stick's Z-axis push. It is stiff, and pressing it
tends to nudge the stick off-centre — awkward as the primary confirm button in the OLED menu.

**Fix.** Retire the stick-click entirely: put **confirm/edit on a real button (C)** and the
**DRIVE↔MENU toggle on D**, leaving `A`=home and `B`=back. Because button remap is web-only by
design, the firmware's default simply never depends on the bad input.

**Takeaway.** Match an action's importance and frequency to the **quality** of its input. A
precise, frequently-used action (confirm) does not belong on the worst button on the device.

---

## 16. Extend the swap model instead of rewriting the motion engine

**Context.** Decision D-A2 says each backend is a fully independent controller — its own position
**and** its own limits/home/speed/step. The plan's design was a `Controller` struct (`AxisConfig cfg;
int pan, tilt; bool homing;`) threaded through `nudge()`, `startHome()`, `updateHoming()` and every
setter — a rewrite of the whole motion engine.

**What was built instead.** Positions were already independent via a **swap model**: one live copy
of the state in globals, with `ctrlSlot[3]` holding each target's snapshot, exchanged on target
change. Extending that struct to carry the eight config scalars too — plus `saveSlot()`/`loadSlot()`
— delivered the same user-visible independence while leaving **every motion function untouched**.
Perfect for an unattended build: a small, reviewable diff instead of a broad refactor.

**The honest cost.** One live copy means only the **active** target runs: an inactive target's home
glide is *paused* and resumes when you switch back, where the `Controller` model would glide both
concurrently. That is a real behavioural difference, so it is written down in `PROJECT.md` §5 and
the plan's §0 rather than glossed as "done."

**Takeaway.** When an existing pattern already solves the smaller version of the problem, extending
it usually beats the textbook refactor — but only if you **name the capability you gave up**. An
architectural shortcut documented is a trade-off; undocumented it is a latent bug report.

---

## 17. Migrate config by making the old key the new key's default

**Problem.** Splitting one shared calibration into three per-target copies (`pmin` → `pmin0/1/2`)
risks the worst possible outcome for the user: a firmware update that looks like it **reset their
calibration**, because the new keys don't exist yet in NVS.

**Fix — one line per field.** Read the legacy key first, then use *that value* as the default for
each per-target read:

```cpp
panMin = prefs.getInt("pmin", panMin);            // legacy (pre-split) value
...
c.panMin = prefs.getInt(ckey("pmin", t), panMin); // per-target, defaulting to legacy
```

First boot: no `pmin0` exists → every target inherits the user's existing window. From then on each
target writes only its own key. No migration pass, no version stamp, no write on boot — and the
legacy key is left untouched, so downgrading the firmware still finds its old value.

**Takeaway.** A defaulting read *is* a migration. Layering "new key, defaulting to old key" gets
you a zero-touch, reversible upgrade path with none of the usual migration-script machinery.

---

## 18. Extending a struct silently zeroes every short aggregate-init elsewhere

**Symptom (caught in review, one flash before it could bite).** `CtrlState` grew from 4 members to
12 when each control target gained its own config (§16). Everything compiled and ran. But a
**pre-existing** line in `setup()`, written when the struct had exactly 4 members, was:

```cpp
for (int i = 0; i < 3; i++) ctrlSlot[i] = { panPos, tiltPos, false, 0 };   // all targets start at home
```

**Diagnosis.** Assigning from a brace-init-list with fewer elements than the struct has members is
**legal C++**: the omitted members are *value-initialized* — i.e. set to **0**. So this line ran on
every boot, immediately after `loadSettings()` had carefully restored and validated each target's
calibration, and zeroed `panMin/panMax/tiltMin/tiltMax/homePan/homeTilt/homeSpeed/stepSize` in all
three slots. No compiler error, no warning, no crash.

The damage was latent, not visible: the *live globals* were still correct, so the rig looked fine.
It would surface only on the first control-target switch, where `loadSlot()` copies a zeroed slot
into the live globals — giving `panMin == panMax == 0` and `stepSize == 0`. The next D-pad press
then computes `constrain(panPos + 0, 0, 0)` → **0**, commanding an immediate full-travel move to 0°;
and `homeSpeed == 0` makes `updateHoming()` latch `homing = true` forever with no motion. Worse, a
user "fixing" the blank fields would persist the zeros over their real calibration.

**Fix.** Assign the intended fields **member-wise** (and clamp each slot's position to its *own*
window, which the brace form also got wrong):

```cpp
for (int i = 0; i < 3; i++) {
  ctrlSlot[i].pan  = constrain(panPos,  ctrlSlot[i].panMin,  ctrlSlot[i].panMax);
  ctrlSlot[i].tilt = constrain(tiltPos, ctrlSlot[i].tiltMin, ctrlSlot[i].tiltMax);
  ctrlSlot[i].homing = false;  ctrlSlot[i].homeLastMs = 0;
}
```

**The permanent guard.** `platformio.ini` now builds with **`-Wmissing-field-initializers`**, which
names the exact omitted members:

```
warning: missing initializer for member 'CtrlState::panMin' [-Wmissing-field-initializers]
```

Verified two ways: the codebase is **clean** under it (zero warnings, so it is pure signal), and
re-planting the truncated pattern makes it fire immediately. It would have caught this at compile
time, for free.

**Takeaways.**
- Adding a field to a struct is **action at a distance**: every existing short aggregate-init of
  that type silently changes meaning. When you extend a struct, grep for *every* `= {` of it.
- Prefer member-wise assignment over brace-lists for "reset a few fields" — brace-lists silently
  mean "and zero everything else."
- Turn the class of bug into a **compiler error, not a memory**. A one-line build flag outlives any
  amount of reviewer diligence.
- Reviewing beats testing for this one: the bug is invisible on the active target and only appears
  after a target switch, so a casual bench test would have passed it.

---

## 19. A crash hidden behind a pipe looks exactly like a hardware hang

**Symptom.** Uploads that had been taking ~40 s suddenly ran past a 300 s timeout with **no output
at all**. `COM11` was present, the board had just been flashing fine, and there were stale
`python.exe` processes around — every sign pointed at a **wedged serial port**, so time went into
killing zombies and re-testing whether the port would open (it did, cleanly).

**Diagnosis.** The port was never the problem. The command was
`pio run -t upload … 2>&1 | tail -4` — and `tail` **buffers everything until the process exits**,
so the failure was invisible. Running it unpiped showed the real error immediately:

```
UnicodeEncodeError: 'charmap' codec can't encode characters in position 23-52
```

PlatformIO was crashing while *printing* esptool's progress bar: the Windows console is **cp1252**
and the bar is drawn with Unicode block characters. The tell had been on screen for the entire
project — the benign-looking `Firmware metrics can not be shown. Set the terminal codepage to
"utf-8"` line in every build.

**Fix.** `export PYTHONIOENCODING=utf-8` before uploading (or `chcp 65001`), and don't funnel
long-running tool output through `tail` — filter with `grep -E "Wrote|Hash|SUCCESS|FAILED"`, which
streams.

**Takeaways.**
- **A pipe that buffers converts a crash into a hang.** If a command "hangs" with zero output,
  suspect your own plumbing before the hardware — re-run it unpiped first; it costs one command.
- The environment's own warnings are evidence. A cosmetic-looking notice ("set the codepage") had
  been describing the exact failure mode for weeks before it turned fatal.
- Same trap as §13: the *observable* was misleading, and the fix was to establish what was actually
  being measured rather than theorise from a symptom.

---

## 20. D1 RESOLVED: `pinMode()` detaches a pin from its peripheral on ESP32 core 3.x

**Symptom.** With an LX-16A wired to the bus pin, **nothing** worked: no telemetry, no bus scan,
no motion — not even a broadcast (ID 254) move, which needs neither reads nor the servo's ID.
Fault read `136` = `0x88` = "bus dead" on both servo IDs.

**The wrong turns (worth recording).** Power was suspected first — the servo was running off the
ESP32's 5 V, below its 6 V minimum, and an unpowered LX-16A clamps the bus line to ground, which the
pin test saw. That was a real fault and worth fixing. But after an external supply, a verified common
ground, and a correct signal wire, it *still* did nothing — and the next instinct was to keep
doubting the wiring. Every downstream check had passed; the one thing never verified was **our own
end of the link**.

**The measurement that cracked it.** Instead of testing the servo, test the transmitter: sample the
pad while a packet goes out. The pad drove cleanly high and low as a plain GPIO (so the pin was
healthy), but **the UART left it low for 5 ms after init and it never toggled during a
transmission**. The ESP32 had never sent a single byte.

**Root cause.** `lx16a-servo` flips bus direction around every write (`lx16a-servo.h:117/136`):

```cpp
pinMode(myTXPin, OUTPUT|PULLUP);   // before write
_port->write(buf, buflen);          // ...send
pinMode(myTXPin, INPUT|PULLUP);     // release for the reply
```

On **arduino-esp32 3.x, `pinMode()` detaches the pad from its peripheral.** The instant the library
sets the pin to `OUTPUT` to transmit, it takes the pad away from the UART, and `write()` pushes bytes
at a pad the UART no longer drives. On core 1.0.x `pinMode()` didn't detach — which is exactly the
narrow validation `platformio.ini` recorded as deferred decision **D1**.

**Fix.** Never let `pinMode()` touch the bus pin. Open the UART **TX-only** and emit raw packets:

```cpp
Serial2.begin(LX_BAUD, SERIAL_8N1, -1, lxpin);   // RX detached; TX keeps the pad
lxRawSend(id, cmd, params, n);                    // 0x55 0x55 id len cmd ... ~checksum
```

Motion works immediately. **Cost:** replies can't be received, so telemetry, bus scan and ID
readback are unavailable on this core — reported as `--`, never faked, and `telemetry()` returns
`false` so the poller neither burns a bus timeout per tick nor raises a bogus "bus dead" fault
(`0x88` → none).

**Takeaways.**
- **When every downstream check passes, verify your own transmitter.** Three rounds of wiring
  suspicion cost more than one measurement of "are we sending anything at all?" would have. Test the
  end of the link you control first — it needs no cooperation from the hardware under test.
- **On ESP32 core 3.x, `pinMode()` on a peripheral-routed pin silently un-routes it.** Any library
  that toggles direction that way for half-duplex is broken on modern cores, and it fails *silently*
  — no error, no exception, just bytes into the void.
- **Know your instrument's limits.** `digitalRead()` stops observing the real pad once a peripheral
  drives it as an output (the GPIO input buffer is disabled), so "0 transitions" in TX-only mode was
  my meter failing, not the wire. The servo itself was the trustworthy instrument.
- A "harmless" spec violation can mask the real bug: the 5 V supply produced a genuine, visible
  symptom (line clamped low) that looked like *the* explanation and delayed finding the actual one.

---

## 21. The fix that planted six crashes — and the audit that turned them into working reads

**Symptom.** After the TX-only D1 fix (§20), the user reported "the board just keeps looping."
Serial was silent for 25 s and `/status` answered normally — yet `pan` had reset to home, proving
reboots *had* happened. An intermittent, interaction-triggered loop, not a continuous one.

**Reproduction, then root cause.** With a serial logger attached, `GET /lxprobe` crashed the board
on demand: `LoadProhibited`, backtrace decoded to `LX16ABus::available()` dereferencing a NULL
`_port`. The §20 rewrite had made `Lx16aBackend::begin()` stop initializing the *library's* bus
object — but six call paths still used that library when `bknd==1`: `/lxprobe`, `/lxwiggle`,
`/servoscan`, `/servocfg`, `/servoid`, and the OLED **Servo page**. Each was a crash-reboot. The
diagnostic endpoints even had bring-up guards written `if (bknd != 1 && ...)` — *skipping* init in
exactly the state that needed it, because they assumed `begin()` still did it. **A fix that changes
an initialization contract must visit every consumer of the initialized object.**

**The audit.** A 14-agent workflow (7 dimension-focused reviewers, each finding adversarially
verified against the source) confirmed 57 findings — the crash family plus, among others:
- **Sensor failure decoded as input** (critical): the joystick's `miniRead()` returned `0xFF` on a
  failed I2C read, which decodes as *full-stick deflection* — a loose wire or servo EMI became
  uncommanded max-rate motion, and could even walk the OLED menu to the crashing Servo page with
  nobody touching anything. That closed the case on the "spontaneous" loop: the boot log later
  showed the joystick physically dropping on and off the bus. **A failure value must be out-of-band
  (`-1`), never a legal reading (`0xFF` = a real full-right stick).**
- `/lxtx` and `/lxpintest` silently **killed all motion** (their `pinMode`/library calls detached
  the TX pad) and `/lxfix`'s self-test was a **false negative** — `digitalRead` sampled a pad whose
  input buffer was disabled. My own §20 bench confusion, explained.
- A broadcast **EEPROM limit write on every boot** (wear + silently wiping user limits), boot homing
  both rigs to the *active* target's calibration, home-glides converting handler blocking into one
  violent snap, and fault bits latched red for an axis that never had a servo fitted.

**The fix that subsumed them all.** One audit agent produced a source-verified recipe for real
half-duplex on core 3.x: keep **RX permanently attached** to the bus pin
(`Serial2.begin(baud, cfg, rx=lxpin, tx=-1)`), matrix-route **U2TXD to the pad only around each
packet** (`esp_rom_gpio_connect_out_signal`), release with `gpio_set_direction(GPIO_MODE_INPUT)` —
never `pinMode`, which detaches via the peripheral manager. RX hears our own echo (discard exactly
`k` bytes), then the servo's checksummed reply. The library's bus objects were **deleted outright**,
so the null-`_port` class of crash became impossible *by construction* — the compiler finds any
straggler. Verified live: scan found ID 1 with voltage/temp/position, full config readback, live
telemetry in `/status`, closed-loop motion (commanded 90°, measured 88–89°) — and the restored
telemetry **immediately paid for itself**, measuring only ~5.0 V at a servo fed from a "6 V" supply.

**Takeaways.**
- **Deleting the object beats guarding it.** Null-guards at six call sites would have patched the
  symptom; removing the library objects made the entire failure class unrepresentable.
- **Out-of-band failure signalling is a safety property**, not a style choice, anywhere a sensor
  feeds motion. And it is only half the defense: a half-seated module can also return
  **successful reads carrying garbage** — measured on this bench when the rig crept to its soft
  limit unattended ("why is the servo moving?"). The `-1` guard can't see a *successful* lie; a
  **double-read consistency check** can (a real stick can't teleport between two reads ~1 ms
  apart). Validate success, don't just signal failure.
- **A pinned soft-limit is a stall.** Commanded to 120 (its clamp limit) the undervolted servo sat
  at 118 with the motor continuously energized trying to close the gap — the "internal movement"
  hum. At center it tracked exactly (90→90) and rested. Continuous micro-churn at one specific
  angle = the controller pushing against something it cannot reach, and undervoltage widens that
  unreachable zone.
- **An "interaction-triggered" crash and a "spontaneous" one can be the same bug** — it just needs
  a second faulty input (here: a half-seated joystick) to press the button for you.
- Fresh eyes scale: 7 independent reviewers converging on the same root causes is corroboration;
  the one finding none of them duplicated (the half-duplex recipe) was the highest-value output.
- Echo-as-evidence beats pad-sampling: when RX shares the wire, *hearing your own transmission* is
  direct proof the bytes exist, immune to the input-buffer trap that fooled §20's instrument.

---

## 22. Review themes: guards must be uniform, and refactors leak into their consumers

**Context.** A 41-agent review of the finished firmware confirmed 10 distinct correctness bugs.
Almost all of them fell into just two patterns — worth knowing because they predict where the
NEXT bugs will be.

**Pattern 1 — a safety guard added in one place is a bug everywhere it isn't.** The joystick's
failed-read and garbage-read guards were built for the mini stick's axes… and nowhere else: the
(then-present) seesaw axes had none, and the mini's *buttons* had none — so a garbage byte could
still fire a phantom "home" or an EEPROM write. A guard that exists because "input X can produce
uncommanded motion" must be applied to EVERY input that can produce motion, mechanically, not
just where the incident happened. (The seesaw path itself was speculative dead code for hardware
never owned — it was deleted outright; it had cost real review budget to "fix".)

**Pattern 2 — a data-model refactor breaks every consumer that still assumes the old model.**
Making config per-target (`ctrlSlot`) silently invalidated five consumers that assumed one shared
config: "Both" mode clamped by the wrong slot's limits, diagnostic sweeps clamped by whatever
target happened to be active, stall faults comparing one rig's command against the other rig's
telemetry, auto-trim adoption desyncing the mirrored rig, and boot pin-validation reading defaults
that NVS hadn't overwritten yet. After changing what a global MEANS, grep every reader — the
compiler only finds the ones whose types changed.

**Bonus protocol trap:** the LX-16A reports position as **signed** int16. A servo pressed past its
zero end replies −4 ticks; an unsigned decode renders that as ~15,700°, which then poisons the UI,
the fault engine, and the trim loop in one shot. Decode signed, clamp to the physical range.

---

## 23. UI timers must count every kind of user input - and poll-friendly events only

**Symptom (user report).** On the DRIVE page, pressing A (home) "sometimes did nothing, sometimes
quit the page back to MENU."

**Two real defects, one press.**
- The DRIVE page's 5-second idle timeout counted only STICK motion as activity. Pressing A near
  the timeout started the home glide and flipped to MENU in the same tick — and the button's
  feedback flash rendered on the NEW page (whose hint bar has no A), so the press also *looked*
  dead. **Any user input is activity**; and a started glide should keep its page alive until it
  lands.
- Clicks keyed on the module's SINGLE_CLICK event (code 3), a latch that appears only briefly
  after release — a 30 ms poll can straddle and miss it entirely. Keying on the **press-down
  event (code 0)**, which is held-stable for the whole press, cannot be missed at any sane poll
  rate, survives the double-read garbage check naturally, and makes buttons feel snappier
  (respond on press, not release). The fleeting 3 stays only as a fallback for missed taps.

**Takeaway.** When polling event-latch hardware, prefer level-like events (held states) over
edge-like latches; and audit every timeout in a UI for inputs it fails to count — the bug reads
as "flaky button" but is really "the page left while you pressed it."

---

## Meta-lessons

- **Verify with evidence.** Every "is it working?" was answered with a real signal — a serial probe
  (`OLED OK`, `attached=1`), a `netsh` WiFi scan, a PlatformIO compile, hash-verified flashes — not
  an assumption. Two bugs (the vanished AP, the persisted-home drift) were caught precisely because
  a cheap check existed.
- **Read the library, don't guess it.** The SH1107 offset and the ESP32Servo double-attach were
  both settled by opening the source, which turned multi-guess iterations into one correct change.
- **Adversarial review earns its keep on the boring parts.** The persistence bug lived in
  `reclampToLimits()` — exactly the kind of un-glamorous helper a dedicated review lens is for.
- **Confirm the stimulus, not just the measurement.** The joystick "hardware fault" was really an
  uncontrolled input — three captures with nobody touching the stick (§13). Establish the control
  before theorising from the data.
- **Verify with persistent state.** Reading a resting `panPos` after the fact beat racing a live
  capture window to prove the joystick drives the servos (§14). Durable evidence over transients.
- **A compile is not a verification.** An unattended build can end with everything green and
  nothing proven: the board dropped off USB *and* WiFi mid-session, so three finished features
  compile at 86.7 % flash but have never executed. "Compiles" earns the word *written*, not *works* —
  they are logged as **unflashed** everywhere rather than ticked off.
- **When hardware is unavailable, substitute review for testing.** With no board to flash, an
  independent adversarial read of the diff is the strongest verification left — and it targets
  exactly what a bench test would have caught (field-order mismatches, off-by-one CSV indices,
  out-of-range writes).
