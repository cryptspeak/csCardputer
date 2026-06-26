# Theme Configuration

Modules: [`src/ui/Theme.{h,cpp}`](../src/ui/Theme.h)
Wired into: [`src/ui/UIManager.cpp`](../src/ui/UIManager.cpp),
[`src/ui/screens/SettingsScreen.cpp`](../src/ui/screens/SettingsScreen.cpp),
[`src/main.cpp`](../src/main.cpp)

Color theme (9 presets + a custom RGB picker, reachable from
Settings → Display → Theme) is stored in its own file, `theme.json`, on
flash (`/config/theme.json`) and SD (`/ratcom/config/theme.json`) — **not**
inside the encrypted `UserSettings` blob that `SettingsEncryption` protects
(see [encryption-contacts-settings.md](encryption-contacts-settings.md)).

## Menu structure

```
Settings → Display
            ├─ Screen   (brightness, dim timeout, off timeout)
            ├─ Theme    (presets + "[Custom]", see below)
            └─ Name
```

The Theme screen lists "Active: <name>", then every preset, then a final
`[Custom]` row — selected exactly the same way as any preset (same `>[name]`
highlight convention), not a separate "open a submenu" action. Picking
`[Custom]` opens a list of the 7 base hues (current hex shown next to each),
and picking one of those opens the RGB picker described below.

The 7 hues are labeled by what they actually control on screen, not by
abstract design-system terms, and listed in that order in the menu — base
surface/text colors first, then the two emphasis colors, then status colors
in ascending severity (warn before error). This grouping/order and the
label text both live in one place, `kThemeFields[]` in
`SettingsScreen.cpp`, so the menu's display order, its labels, and which
`BaseColors` field each row actually edits can't drift out of sync with
each other:

| Label | `BaseColors` field | What it actually colors |
|---|---|---|
| Background | `bg` | The screen background everywhere. |
| Text | `text` | Main/bright readable text. |
| Subtext | `secondary` | Dimmer secondary text (subtitles, inactive labels). |
| Highlight | `primary` | Selection, active borders/cursors, progress fill — the "this thing is active" color. |
| Header | `accent` | Per-screen header bars/titles, and the status bar's mode text ("STANDALONE" etc). |
| Warning | `warning` | Medium-severity indicators. |
| Error | `error` | Destructive actions, failures, low battery. |

## Why a separate, intentionally-unencrypted file

Two independent reasons, either one would be sufficient on its own:

1. **It isn't sensitive.** Unlike WiFi passwords or TCP hub addresses, a
   color palette reveals nothing about the user, their network, or their
   contacts. Routing it through `AtRestCrypto` would add real cost (key
   derivation, MAC verification, ciphertext envelope) for zero
   confidentiality benefit — see [threat-model.md](threat-model.md) for what
   the encrypted-at-rest layer is actually defending against.
2. **It's needed before encryption is available.** Per
   [boot-sequence.md](boot-sequence.md), `UserConfig`/`SettingsEncryption`
   can't decrypt anything until the identity is unlocked — which happens
   *after* the password-gate screen has already rendered. The boot screen
   and password prompt are themed too, so the theme has to load before any
   domain key exists. Putting it in the encrypted blob would mean the very
   screens that ask for the password couldn't be themed.

## File format

```json
{
  "name": "Custom",
  "bg": "05080A",
  "text": "E4F3F0",
  "primary": "00E06D",
  "secondary": "91A8A5",
  "accent": "4FD7FF",
  "error": "FF5C6C",
  "warning": "F0C04F"
}
```

Seven base hues, each a 6-digit hex string. Every other color used by the UI
(elevated/surface/hover backgrounds, muted text, selection highlight, badge
colors, etc. — ~21 in total) is *derived* from these seven at load time
(`Theme::apply()` in `Theme.cpp`), not stored. This keeps the file small and
means there's no way for a partially-edited file to produce an internally
inconsistent palette (e.g. a selection highlight that doesn't relate to the
primary color at all).

For the exact compiled-in default palette, derivation is bypassed and the
original hand-tuned constants are used verbatim — picking the "Default"
preset (or never touching Theme settings at all) is guaranteed pixel-stable
across this change.

## Presets

`Theme::PRESETS[]` in `Theme.cpp`: Default, Amber Retro, Ocean Blue, Mono,
High Contrast, Voidframe (purple + neon green), Nightshade (layered purple
shades), Phosphor (monochrome CRT green), Glitchline (cyan/magenta duotone).
Adding another preset is a one-line addition to that array — no other code
changes needed, since every other UI color is derived from the same 7 base
hues at load time.

## Custom color entry

`[Custom]` opens a list of the 7 base hues, plus an "Input: Hex"/"Input: Mix"
toggle at the top — picking a hue does one of two things depending on that
toggle:

- **Hex (default).** A plain text field — type a 6-digit hex code
  (`RRGGBB`) and press Enter, exactly like every other editable field in
  Settings (Radio frequency, brightness, volume, etc.). A live swatch
  appears next to the input the moment a complete, valid hex value is
  typed, updating on every keystroke.
- **Mix.** An RGB slider screen (`SettingsScreen::renderMixer`) — `;`/`.`
  switch between R/G/B, `,`/`/` decrease/increase the selected channel, a
  live swatch + hex readout update as you go, Enter saves, Esc/Del cancels.
  A tap always moves a channel by exactly 1; holding ramps the step size up
  the longer the key stays down — see "Accelerating hold-to-repeat" below.

Both are reached the same way — selecting a hue row — so "Mix" is purely an
alternative the toggle switches you into, not a separate flow to discover.

### Accelerating hold-to-repeat (Mix mode)

A single tap of `,`/`/` nudges the channel by exactly 1 — precise, ungapped
control. Holding the key down ramps *both* the repeat rate and the step
size up together the longer it stays held (deliberate single nudges →
faster, slightly larger nudges → a fast-but-still-gradual cruise — see
below for why the step grows at all, and why it deliberately stays well
short of a full RGB332 bucket per press), instead of forcing one keypress
per unit of change across the whole 0-255 range. This
needed continuous polling, not the discrete key-event dispatch used
everywhere else in Settings: `Keyboard::isKeyPressed()` was added as a
level (not edge) check of the physical key state, and `Screen::tick()` — a
new no-op-by-default virtual hook called every main-loop iteration via
`UIManager::tick()`, independent of render timing — polls it in
`SettingsScreen::tick()` to drive the ramp. The first step of a hold still
comes from the normal edge-triggered key event (instant feedback on a tap,
always magnitude 1); `tick()` only takes over for sustained holds, via
`mixerStepSize()` for the step and the same held-duration tiers for the
repeat interval.

An earlier version of this screen used the accelerating slider as the
*only* way to enter a custom color, with no direct hex input. That was
slower than just typing on a device with a full keyboard, so hex entry was
restored as the default and the slider was kept only as an opt-in
alternative for mixing by feel.

### Why the step size ramps up at all

The main UI canvas (`UIManager::_canvas`) is created with
`setColorDepth(8)`. That call only enables an actual indexed palette when
`bpp < 8` (see `LGFX_Sprite::setColorDepth(uint8_t)` in M5GFX) — at exactly
8 it renders as **RGB332 truecolor** instead: 3 bits red, 3 bits green, 2
bits blue. `UIManager::refreshPalette()`'s `setPaletteColor()` calls are
therefore silent no-ops (no palette was ever allocated); every `Theme::`
value is truncated straight to its nearest RGB332 bucket at draw time.

A 2-bit channel has only 4 distinct levels, 64 apart. `Theme::ACCENT`'s
default blue value is already 255 (the top bucket), so a held-down,
fixed-step-of-1 repeat could spend dozens of steps inside the same bucket
with no visible color change — this is what made the Header color look
like it "wasn't changing" while mixing. The fix isn't to make every press
big — an earlier version topped the step out at a full bucket width (32/64)
on a fast cruise, which felt like it was teleporting and skipped over most
values. The step only needs to be big enough that a handful of presses'
*accumulated* movement crosses a bucket within a fraction of a second, not
that every individual press does. So it stays deliberately gentle: taps and
short holds move by exactly 1 (precise, ungapped), and `mixerStepSize()`
only grows it to 2/4/8 (R/G) or 4/8/16 (B) on the same 500/1500/3000ms
tiers `tick()` already uses for repeat rate — fast enough to feel like
acceleration, far short of a single press ever covering a whole bucket.
This only affects the slider's step
size, not stored precision — hex entry still accepts and saves the exact
value typed, and the same RGB332 truncation applies to it (and to every
other color in the app) at render time regardless.

## Why a malicious or corrupted file can't do anything beyond look ugly

This was the explicit design constraint: adding a new file that the
firmware parses unauthenticated, at boot, before any password gate, is only
safe if there is no path from file content to anything other than a pixel
value. Concretely:

- **Fixed paths, not file-driven.** The two paths read are compile-time
  constants (`PATH_THEME_CONFIG`, `SD_PATH_THEME_CONFIG`); nothing in the
  file content influences what file gets opened. No path traversal surface.
- **Bounded, strictly-validated fields.** Each of the 7 color fields must be
  *exactly* 6 hex digits or it's rejected — falling back independently to
  whatever that one field's previous value was (so a corrupt `"primary"`
  field doesn't blank out the rest of the theme). There is no parsing path
  that turns file bytes into a buffer length, array index, or pointer
  offset — only into a `uint8_t` 0-255 per channel, clamped by construction
  (a hex byte literally cannot exceed 0xFF).
- **Size-capped before parsing.** Files over 2KB are rejected outright
  before `ArduinoJson` ever runs — the real payload is ~150 bytes, so this
  only matters as cheap defense against a deliberately oversized file
  wasting parse time/heap; it is not load-bearing for correctness.
- **No code path, no `eval`, no format strings.** The values are RGB565
  integers handed straight to `M5Canvas::setPaletteColor()` / used as draw
  colors. There is nothing downstream that interprets them as anything
  other than a color.
- **Fail-safe, not fail-open.** Any parse error, oversized file, or missing
  file silently falls back to the compiled-in default palette
  (`Theme::resetToDefaults()`) — a broken `theme.json` degrades to "ignore
  it," never to a crash or a hang.

In short: the worst outcome of editing this file by hand (or maliciously) is
an ugly or low-contrast UI. It cannot be used to escalate to reading
encrypted data, executing code, or affecting any other subsystem.

## Boot-time loading

`Theme::load()` is called twice in `main.cpp::setup()`, mirroring the
"SD primary, flash fallback" pattern used elsewhere in the storage layer:

1. **Flash-only, before `ui.begin()`.** `flash.begin()` is pulled forward
   (it's idempotent — see `FlashStore::begin()` — so the existing later call
   and its boot-screen progress step are unaffected) so a saved theme can be
   applied before the very first frame, including the boot screen and
   password gate.
2. **SD-aware, after `sdStore.begin()`.** SD shares an SPI bus with the
   radio and isn't mounted until after the boot screen has already started
   rendering, so it can't be checked in step 1. Once mounted, `Theme::load()`
   runs again; if SD has a saved theme it takes priority over flash (same
   tier ordering as `UserConfig`), and `UIManager::refreshPalette()` repaints
   immediately if anything changed.

Neither call touches `rns`, `identity`, or any encryption state — see
[boot-sequence.md](boot-sequence.md) for why that ordering matters for
*other* domains and why it deliberately does not apply here.
