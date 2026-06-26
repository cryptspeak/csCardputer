# Changelog

All notable changes to Cryptspeak (rsCardputer-CE) are documented here.
Versioning is independent of upstream
[ratspeak/rsCardputer](https://github.com/ratspeak/rsCardputer) — see
`docs/firmware-architecture.md`.

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
