# ESP32 Pan/Tilt — session handover

Read `lessonlearn.md` before debugging anything: 23 hard-won lessons, each one cost real time.
`PROJECT.md` = full technical design. `serial_upgrade_and_joystick.md` §0 = feature status table.

## Bench facts (not derivable from code)

- **COM11 = the ESP32** (COM3 is Intel-AMT — ignore). Flash: `pio run -t upload --upload-port COM11`.
- **`export PYTHONIOENCODING=utf-8` before every upload from bash** — otherwise esptool's Unicode
  progress bar crashes PlatformIO on the cp1252 console, and a `| tail` pipe makes the crash look
  like a serial-port hang (lesson 19). Filter upload output with `grep -E "Wrote|SUCCESS|FAILED"`.
- Device joins SSID **"1TakePass"**, lately at **192.168.14.110** (DHCP; OLED Connection page shows it).
- **Known bench gremlin:** the board has spontaneously lost USB/power 3× (twice fully, once as a
  mid-test reboot). Nothing remote fixes it — the user must replug. A failed upload writes nothing.
- The user's "6 V" supply measures **~5.0 V at the servo** (status field 16; fault bit 0x02 is real).
  Target is 7.4 V. Their joystick wiring has been flaky — firmware tolerates it, but reseating helps.
- One LX-16A at **ID 1** on GPIO 32. Second servo owned but not yet programmed (workflow: manual §8).

## Invariants — violating any of these re-introduces a fixed bug

- **NEVER call `pinMode()` on the LX bus pin** (`lxpin`). On core 3.x it detaches the pad from the
  UART via the peripheral manager — this exact mistake made the lx16a-servo library transmit
  nothing (lessons 20–21). All bus I/O goes through `lxBusBegin`/`lxRawSend`/`lxRawRead`
  (GPIO-matrix half-duplex; RX permanently attached, TX routed per packet). The lx16a-servo
  library is included for its command constants ONLY — never instantiate its bus objects.
- **Status CSV is append-only** (35 fields). `fullStatus()` emission order and `applyState()`
  indices in `web_page.h` must stay in lockstep — an off-by-one silently corrupts the whole UI.
- **`ctrlSlot[3]` swap model:** live globals always belong to the ACTIVE target; the slot arrays
  are stale for the active target until `saveSlot()` runs. Only the active target glides. Any new
  consumer of limits/home/speed must decide slot-vs-live deliberately (lesson 22, five bugs).
- **Joystick reads are safety-critical:** failed reads return −1 (never a value), axes are
  double-read for consistency, buttons fire on the press-down edge with a confirm read. Any new
  input path that can cause motion gets the same guards (lesson 22).
- **LX POS_READ ticks are SIGNED int16** — decode via `lxTicksToDeg()`, never raw.
- `-Wmissing-field-initializers` is enabled and the build is clean under it — keep it that way
  (a truncated brace-init once zeroed all per-target calibration at boot; lesson 18).
- Arduino builder traps: functions taking user structs need a **manual prototype** after the
  struct; globals/arrays are **not** auto-hoisted — declare before first use.
- Servo EEPROM writes (angle limits, trim-save, ID) are wear-limited and user-visible — never put
  one in a boot path or a per-step edit loop.

## Verification workflow that works here

1. Compile+flash (see bench facts), wait ~12 s, then `curl http://<ip>/status`.
2. Boot serial via pyserial (PlatformIO's penv python) with `dtr=False, rts=False`; pulse RTS to reset.
3. Hardware truths: prove motion via **persistent state** (positions survive; read after, don't
   race a capture window — lesson 14); prove TX via **RX echo** (`/lxtx`), never `digitalRead` on a
   peripheral-driven pad (lesson 20); confirm the physical stimulus before trusting any measurement
   (lesson 13). Diagnostics live at `/lxprobe /lxtx /lxpintest /lxwiggle /lxfix /lxcal /servotest`.

## Publishing pipeline

- GitHub: https://github.com/siujacky/esp32-pan-tilt (public, MIT). Commit style: what+why,
  `Co-Authored-By: Claude` footer. Push after hardware verification, not before.
- Manual (3 channels, keep in sync): `docs/manual.html` in-repo (standalone HTML → GitHub Pages at
  https://siujacky.github.io/esp32-pan-tilt/manual.html) · release asset
  (`gh release upload v1.0.0 docs/manual.html --clobber`) · claude.ai artifact
  https://claude.ai/code/artifact/6f9b5f4a-4dea-4d00-8bd4-89e50a3e51a6 (publish an UNWRAPPED copy —
  no doctype/html/body — via the `url` param). OLED figures in it are hand-drawn SVGs matching the
  draw code; update them when screens change.

## Open work, in rough value order

1. **Position presets** (save/recall framing shots) — tracked as an open task; the natural next
   feature. Flash headroom is ample now (61 % of 1.97 MB).
2. **Second LX servo bring-up** when the user is ready: program ID 2 (web Serial→IDs, lone flow),
   link tilt, set its limits. Everything is built and waiting.
3. **7.4 V supply** — watch `/lxcal` trims decay to ~0 and fault 0x02 clear as confirmation.
4. Settings export/import; auth if it ever leaves the LAN.
5. Watch for the bench power gremlin (now mostly defanged: **prefer OTA flashing** —
   `pio run -e esp32ota -t upload`, target `pantilt.local`).

Done & hardware-verified 2026-08-09 (2nd wave): mDNS (`pantilt.local`), OTA end-to-end (wireless
flash → reboot → healthy), and the collision watch physically tripped via a synthetic obstacle
(servo EEPROM limit): trip at 84°, backoff +5° opposite travel, HALT with total motion lockout,
clean full sweep after restore.
