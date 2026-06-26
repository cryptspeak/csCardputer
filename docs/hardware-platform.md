# Hardware & Platform Layer

Modules: [`src/hal/`](../src/hal/), [`src/input/`](../src/input/),
[`src/power/PowerManager.{h,cpp}`](../src/power/PowerManager.h),
[`src/audio/AudioNotify.{h,cpp}`](../src/audio/AudioNotify.h),
[`src/platform/RsCardputerModeSwitch.h`](../src/platform/RsCardputerModeSwitch.h),
[`src/config/BoardConfig.h`](../src/config/BoardConfig.h)

The board-level glue that doesn't belong to Reticulum, storage, or UI.
All pin assignments live in `BoardConfig.h` — this doc explains the
*why* behind the ones that aren't self-explanatory; see that file for
the full pinout.

## GPS (`hal/GPSManager.{h,cpp}`, `hal/NMEAParser.h`)

**Status: backend implemented, not yet exposed in the Settings UI.**
`UserConfig`'s `gpsTimeEnabled`/`gpsLocationEnabled` fields exist and
default to `true`/`false` respectively, and `main.cpp` wires up
`GPSManager` from them at boot, but there is currently no Settings
screen to toggle either one — they're only reachable by hand-editing
the saved `user.json` config. `BoardConfig.h` marks the GNSS pins
"reserved for v1.1" — that version label is inherited from the
original upstream codebase this firmware was forked from (this fork
uses its own independent release line, see `Config.h`), so treat it as
"not yet finished," not as a commitment from this fork's maintainer.

Reserved for the optional Cap LoRa-1262 GNSS module on UART2
(`GPS_RX=15`, `GPS_TX=13`, `BoardConfig.h`). `NMEAParser` is a small,
allocation-free, character-at-a-time `$GxRMC`/`$GxGGA` sentence parser
(handles `GP`/`GN`/`GL`/`GA`/`GB` talker-ID prefixes) — there's no NMEA
library dependency for this.

Two independent things come out of a fix, tracked separately because
they become available at different times:

- **Time** — parsed from every valid RMC sentence regardless of fix
  quality, because a sentence carries UTC time even before the receiver
  has a usable position lock. This is also why `setLocationEnabled()`
  defaults to **off**: a user who only wants GPS for time sync (no
  location stored anywhere) doesn't pay for position parsing at all.
- **Location** — only parsed if `setLocationEnabled(true)` was called
  *and* the sentence's status field is `A` (active fix, not `V` for
  void).

`GPSManager::begin()` auto-detects baud rate (tries 38400, 115200, then
115200's `GPS_BAUD` default again — see the source for the exact
order) since not every GNSS module variant ships at the same default
rate; it gives up and stays on the last-tried rate if none produce a
parseable sentence within the detection window. Time is persisted to
NVS (epoch seconds; the comment notes the location fields are stored
as scaled integers — microdegrees / centimeters — specifically to
avoid float-in-NVS edge cases on this platform) so a power-cycle can
restore a "last known" time/position before the GPS module re-acquires
a fix, without claiming GPS-grade accuracy for that restored value (the
restore path does not mark a fresh satellite fix as having occurred).

## Keyboard (`input/Keyboard.{h,cpp}`)

Wraps M5Cardputer's hardware keyboard matrix (TCA8418 controller over
I2C, `KB_INT=11` active-low falling-edge interrupt). `Keyboard::update()`
is interrupt-gated with a 250ms fallback poll, so a missed interrupt
can't permanently stall input — at worst, input lags by up to one poll
interval. Two independent input surfaces:

- **Edge-triggered events** (`hasEvent()`/`getEvent()`) — one `KeyEvent`
  per physical key transition, the normal path used for navigation,
  typing, and hotkeys.
- **Level-triggered state** (`isKeyPressed(char)`) — added specifically
  for the theme color mixer's accelerating hold-to-repeat control (see
  [theme-config.md](theme-config.md#accelerating-hold-to-repeat-mix-mode)),
  which needs to know "is this key *still* down right now," not just
  "did it just transition."

`InputMode` (`Navigation` vs `TextInput`) is a hint the UI layer sets
to change *how a key is interpreted downstream* — the keyboard driver
itself always reports the same hardware events either way. Caps lock on
this hardware is a momentary "Aa" key, not a physical latch, so the
firmware forces it back off after every keypress to stop the
underlying M5Cardputer library from latching state the hardware
doesn't actually have a toggle for.

## Hotkeys (`input/HotkeyManager.{h,cpp}`)

Ctrl+`<letter>` global shortcuts, checked in `main.cpp`'s key-handling
chain **before** the active screen gets the event — see
[ui-framework.md](ui-framework.md#input-routing) for exactly where this
sits relative to the help overlay and tab-cycling. Registration is a
flat `name → callback` map built once at boot
(`hotkeys.registerHotkey('h', "Help", onHotkeyHelp)`, etc. in
`main.cpp`); there's no unregister path because the hotkey set is
static for the firmware's lifetime. Tab-cycling (`,`/`/`) is handled
separately, directly in `main.cpp`, specifically *after* the active
screen has had a chance to consume the keypress for its own purposes —
`HotkeyManager` only owns the Ctrl-modified shortcuts.

## Power management (`power/PowerManager.{h,cpp}`)

Three states, driven purely by elapsed time since the last
`activity()` call:

```
ACTIVE ──(idle ≥ dimTimeout)──▶ DIMMED ──(idle ≥ offTimeout)──▶ SCREEN_OFF
  ▲                                                                  │
  └──────────────────────── activity() ─────────────────────────────┘
```

`activity()` always updates the last-activity timestamp, regardless of
current state, and additionally forces an immediate transition back to
`ACTIVE` if the screen wasn't already there — so a single keypress
during `DIMMED` both wakes the display *and* resets the idle clock in
one call. A timeout of `0` disables that transition entirely (e.g. "off
timeout: never"). Brightness during `DIMMED` is a fixed `DIM_BRIGHTNESS`
(64/255, 25%) rather than a configurable value — only the full/`ACTIVE`
brightness is user-adjustable from Settings.

## Audio (`audio/AudioNotify.{h,cpp}`)

A small non-blocking note-sequencer over M5Unified's `Speaker` (ES8311
codec + NS4150B amp). Each notification is a short, fixed sequence of
`{frequency, duration, gap}` triples (max 8 notes) — boot (4-note
ascending chime), message (double-beep), announce (single tick), error
(triple beep). "Non-blocking" matters specifically because the message
sound has to be triggerable from contexts (packet callbacks → main
loop) where a blocking `delay()`-based tone would stall Reticulum
processing — see the `pendingMessageSound` flag in `main.cpp`, which
defers the actual `audio.playMessage()` call out of the packet
callback and into the next `loop()` iteration specifically for this
reason.

## Launcher mode switch (`platform/RsCardputerModeSwitch.h`)

This firmware is one of (at least) three OTA app images sharing the
device's flash — Launcher, Standalone (this firmware), and RNode —
selected via ESP-IDF's OTA boot-partition mechanism
(`esp_ota_set_boot_partition()`), not a reboot-into-bootloader scheme.
`setNextBoot(mode)` only **arms** which partition the *next* reset will
load; it does not reboot by itself.

`main.cpp::setup()` calls `returnToLauncherNextBoot()` unconditionally,
as the very first thing it does on every boot
([main.cpp:963](../src/main.cpp)) — before touching the display,
keyboard, radio, or anything else. Read together with the
boot-loop-counter logic right below it in the same function (which
separately forces WiFi off after 3 consecutive failed boots), this
reads as a safety net: if Standalone crashes, watchdog-resets, or
browns out before it can finish booting, the *next* power-on lands at
the Launcher menu instead of silently looping back into a possibly-
broken Standalone image. The Launcher side of this (re-arming
next-boot to Standalone when the user picks it from the menu) lives in
a separate firmware image, not in this repo.

## Pin reference

| Peripheral | Signal | Pin | Note |
|---|---|---|---|
| SX1262 LoRa | SCK / MISO / MOSI | 40 / 39 / 14 | FSPI/SPI2, shared with SD |
| SX1262 LoRa | CS | 5 | |
| SX1262 LoRa | IRQ (DIO1) | 4 | Falling edge |
| SX1262 LoRa | RST | 3 | Active low, 100µs pulse |
| SX1262 LoRa | BUSY | 6 | Poll before every SPI transaction |
| SD card | CS | 12 | FSPI/SPI2, separate CS from radio |
| Keyboard (TCA8418) | SDA / SCL | 8 / 9 | I2C |
| Keyboard (TCA8418) | INT | 11 | Active low, falling edge |
| Cap LoRa-1262 RF switch (PI4IOE5V6408) | I2C addr | 0x43 | Pin P0 must be HIGH for TX/RX — see `enableCapLoRaRfSwitch()` in `main.cpp` |
| GNSS | RX / TX | 15 / 13 | UART2, baud auto-detected |
| Display | — | — | M5Unified-managed, HSPI/SPI3 (separate bus from radio/SD) |
