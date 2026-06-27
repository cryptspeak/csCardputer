# Changelog

All notable changes to Cryptspeak (rsCardputer-CE) are documented here.
Versioning is independent of upstream
[ratspeak/rsCardputer](https://github.com/ratspeak/rsCardputer) — see
`docs/firmware-architecture.md`.

## v0.0.5 — Radio Frequency Fix

### Fixed

- **Radio:** switching the LoRa frequency to a new band (e.g. the
  default Americas 915MHz to a saved Europe 868MHz setting on boot)
  could silently fail to reach the chip — the radio kept transmitting
  and receiving on the old frequency while software, logs, and the
  Settings screen all showed the new one as active. Reopening Settings
  and confirming any radio value "fixed" it until the next reboot, which
  masked the real bug: `calibrate_image()` skips recalibration once a
  band has already been calibrated, so that second call never actually
  exercised the broken path, it just reapplied the frequency on an
  already-calibrated, idle chip.
  The actual cause was `waitOnBusy()` capping its poll at 100ms.
  `CalibrateImage` on a fresh, uncached band can legitimately take
  longer than that on this hardware, and once the cap was hit the
  function returned while the chip's BUSY line was still genuinely
  asserted. The SX126x ignores SPI while busy, so the `SetRfFrequency`
  write issued right after by `setFrequency()` landed on a still-busy
  chip and was silently dropped, leaving the old frequency in effect.
  Fixed by raising the `waitOnBusy()` cap to 1000ms and adding the same
  settle delay `calibrate()` already had before its own `waitOnBusy()`
  call, since `CalibrateImage` has the same BUSY-assertion-propagation
  gap.

Full diff: https://github.com/0x00001312/rsCardputer-CE/compare/v0.0.4...v0.0.5

## v0.0.4 — Boot Branding, Radio & Transport Fixes

### Added

- Boot screen now opens with a short Cryptspeak intro animation: the
  speech-bubble/keyhole mark wipes in top-to-bottom, "CRYPTSPEAK"
  decrypts into place letter-by-letter through scrambled noise glyphs,
  then the subtitle/version/progress bar fade in together instead of
  popping in at full brightness. The mark is drawn with the live theme
  accent color, so it matches whatever preset is active. Adds a fixed
  one-time ~1s cosmetic delay at the very start of boot; nothing else in
  the boot sequence is reordered or delayed.
- LoRa interface now defaults to Reticulum's roaming mode instead of
  the library default, matching this firmware's use as a mobile
  endpoint rather than a fixed gateway.

### Fixed

- **Radio:** image/PLL calibration was issued from the wrong standby
  mode (`STDBY_XOSC` instead of the datasheet-required `STDBY_RC`)
  whenever the configured LoRa region changed (e.g. Americas 915MHz →
  Europe 868MHz). This left the chip locked on the previously
  calibrated band until a happenstance no-op Settings save "fixed" it
  — a real bug that's been present since the radio driver was written.
- **Radio:** `isTxBusy()` checked the timeout deadline before checking
  the TX-done IRQ flag, so a `loop()` call delayed past the deadline by
  UI rendering could discard a transmission that had actually completed
  successfully. The radio driver now checks the IRQ first, and a
  genuine TX timeout is retried once automatically instead of silently
  dropping the packet. Also logs if the SPI mutex fails to allocate,
  which previously failed silently.
- **Transport:** `TCPClientInterface`'s RX/TX frame buffers were
  `static`, shared across every instance under the assumption only one
  TCP hub connection runs at a time. Since up to 4 hub connections can
  run concurrently, two in-flight connections could silently corrupt
  each other's buffered bytes. Buffers are now per-instance. Also fixed
  Header2 hub `transport_id` learning, which was cached once from the
  first packet and never re-checked — if a hub later regenerated its
  identity without dropping the TCP socket, outgoing packets kept using
  the stale id.
- **UI:** password/unlock screen had a dead backspace handler (an
  unreachable code path meant to return focus to the password field
  from an empty confirm field) and a layout bug where setup mode's
  two-field box overlapped the status/hint text. Both fixed, and field
  labels are now left-aligned to their box instead of floating centered
  above it.

### Removed

- The M5Launcher boot-mode chooser and the bundled GPLv3 RNode TNC
  firmware image, along with the dual-image OTA partition-switching
  code that supported them. This fork only ever ships the standalone
  LXMF messenger, so the dual-image infrastructure inherited from
  upstream was dead weight — and the OTA mode-switch code was actively
  harmful, since it unconditionally forced the boot partition back to
  slot 0 on every boot. Does not affect RNode on-air protocol
  compatibility; the SX1262 driver and LoRa transport used by
  standalone mode are untouched.
- Dead code: an unused `HotkeyManager` tab-cycle callback that was
  never invoked, and a `UIManager` render flag that was written once
  and never read.

### Docs

- Added six technical docs covering the Reticulum/LXMF core, every
  network transport interface, the SX1262 radio driver, the
  hardware/peripheral layer, and the UI framework — filling in the
  gaps left by the existing encryption/storage docs.
- Fixed the `Makefile`/`merge_firmware.py` build-and-package flow,
  which never actually worked under a pipx-installed PlatformIO.
  `make package` now produces `dist/rscardputer-standalone.zip`
  end to end.

Full diff: https://github.com/0x00001312/rsCardputer-CE/compare/v0.0.3...v0.0.4

## v0.0.3 — Custom Color Themes

### Added

- Custom UI color themes, configurable from Settings → Display → Theme.
  9 built-in presets (Default, Amber Retro, Ocean Blue, Mono, High
  Contrast, Voidframe, Nightshade, Phosphor, Glitchline) plus a `[Custom]`
  entry, chosen the same way as any preset.
- Two ways to set a custom color: type a 6-digit hex code directly, or use
  an optional RGB mixer with accelerating hold-to-repeat — a tap moves a
  channel by exactly 1, holding ramps both the repeat rate and step size
  together for a fast but still controllable sweep.
- Theme data lives in its own file, `theme.json` (flash + SD), deliberately
  outside the encrypted settings blob — color data isn't sensitive, and it
  has to be available before the password gate even renders. Strict
  per-field validation and a 2KB size cap mean a corrupted or maliciously
  edited file can never do worse than look ugly. Full design and threat
  reasoning: [`docs/theme-config.md`](docs/theme-config.md).

### Changed

- Settings reorganized: Theme moved under Display; brightness/dim/off
  timeout moved into a new Display → Screen submenu.
- Removed the radio frequency presets (bloat) — radio fields are plain
  manual entry only now.
- The 7 editable theme fields are renamed and reordered to describe what
  they actually control rather than abstract design-system terms:
  Background, Text, Subtext, Highlight, Header, Warning, Error.

Full diff: https://github.com/0x00001312/rsCardputer-CE/compare/v0.0.2...v0.0.3

## v0.0.2

- Added the [Web Flasher](https://0x00001312.github.io/CryptspeakFlasher/)
  as the recommended way to flash release builds — no local PlatformIO
  toolchain needed.

## v0.0.1 — Independent Release Line

First release under Cryptspeak's own versioning, decoupled from upstream
ratspeak/rsCardputer's numbering and the old `-CE` tag suffix.

- **Security:** fixed a critical Ed25519 signature-verification bypass
  inherited from upstream microReticulum (identity spoofing risk).
- **Encryption:** closed the two remaining at-rest gaps — contacts and
  device settings are now encrypted with the same AES-256-CTR + HMAC-SHA256
  construction already used for identity and messages, via a shared
  `AtRestCrypto` engine.
- **Branding:** product renamed to Cryptspeak on-device and in docs;
  `rsCardputer-CE` kept as the underlying codebase name.
- RNode: hardened SX1262 host-reconnect handling.

Full release notes: https://github.com/0x00001312/rsCardputer-CE/releases/tag/v0.0.1
