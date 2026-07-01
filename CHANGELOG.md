# Changelog

All notable changes to Cryptspeak (csCardputer) are documented here.
Versioning is independent of upstream
[ratspeak/rsCardputer](https://github.com/ratspeak/rsCardputer) — see
`docs/firmware-architecture.md`.

## v0.1.2 — TCP Stability, Name Persistence & Identity Verification

### Fixed

- **TCP:** toggling TCP connections in Settings crashed the device with a
  double-free/use-after-free. `retireTCPClient()` called `delete` on a raw
  pointer already owned by `RNS::Interface`'s shared_ptr; teardown then freed
  it a second time. Fixed by letting `tcpIfaces.clear()` be the sole
  destructor and calling `tcp->stop()` directly before clearing both
  containers.
- **TCP:** `TCPClientInterface::stop()` returned early when WiFi dropped
  while a connect task was still in flight, leaving the task alive with a
  dangling `self` pointer. If the destructor ran first, the task's subsequent
  `_connectState` write hit freed memory. Fixed by always waiting for the
  connect task; ESP32 lwIP aborts pending sockets within milliseconds of
  network loss, so the wait is fast.
- **TCP:** a reloaded `TCPClientInterface` inherited stale HDLC frame state,
  hub transport ID, and reconnect backoff from its previous run. `start()` now
  resets all per-connection state. A re-announce fires 4 s after new clients
  are created so the hub learns our address on the fresh connection.
- **Names:** non-contact peer names disappeared after every reboot. Three
  interlocking bugs: (1) no priority tracking — TCP hub announces could fill
  the 60-entry on-disk cap, displacing peers we'd actually messaged; (2)
  eviction race — the RAM guard could evict a name before the first message
  exchange marked it persistent, with no recovery path; (3) missing dirty flag
  — if the 5 s save timer fired before a message exchange, `markNamePersistent`
  returned early without scheduling a re-save, so the entry was never promoted
  to a guaranteed slot. All three fixed; names of messaged peers now survive
  reboots reliably regardless of TCP hub activity or session length.
- **Names:** `lookupName()` short-circuited on the 12-char truncated-hash
  placeholder assigned by `notePeerInterface()` when a peer messages us before
  ever announcing, blocking `recall_app_data()` from returning their real
  display name from the RNS identity cache.
- **Stamps:** stamp computation could grind forever or crash when a peer
  required a high proof-of-work cost. Added `STAMP_HARD_MAX=16` (hard reject
  above the ESP32-S3 feasibility ceiling), a 30 s abort timeout, and
  `try/catch` OOM protection in the stamp task. Task stack doubled to 8 KB.
  UserConfig ceiling clamped to 1–16 to match.

### Added

- **UI:** "Show Address" option in both the Peers tab action menu and the
  Messages tab context menu. Displays the peer's full 32-char LXMF destination
  hash as two formatted lines (`abcd ef01 2345 6789` / `89ab cdef 0123 4567`),
  making it straightforward to compare against a known-good hash and detect
  identity spoofing.

### Changed

- **Settings:** WiFi mode and Auto-discover LAN changes now chain a
  "Reboot now / Later" popup immediately after saving, replacing an
  easily-missed toast. Both settings require a reboot to take effect.
- **Peers:** non-contact peers are now sorted by most-recently-heard time
  (most recent first) instead of alphabetically, so active nodes float to
  the top of the list. Saved contacts keep alphabetical order.

Full diff: https://github.com/cryptspeak/csCardputer/compare/v0.1.1...v0.1.2

## v0.1.1 — Repo Move

Repo transferred from the original maintainer's account to the
cryptspeak org and renamed rsCardputer-CE → csCardputer, another step
in growing independent of [ratspeak/rsCardputer](https://github.com/ratspeak/rsCardputer),
the project this started as a fork of. No functional changes —
branding, docs, and dependency links updated to match.

Full diff: https://github.com/cryptspeak/csCardputer/compare/v0.1.0...v0.1.1

## v0.1.0 — Duress Password, Auto-Lock, Anti-Spam Stamps & Radio Reliability

### Added

- **Security:** optional duress password (Settings > Security > Duress
  Password). Entering it at the boot unlock screen instead of the real
  password wipes the identity, messages, contacts, and settings on every
  storage tier (NVS, SD, flash), then reboots straight into the normal
  first-time-setup screen with no "wiped" indicator shown. The new
  `Duress` module stores a verifier blob in NVS and checks candidate
  passwords by reusing `IdentityCrypto`'s wrap/unwrap envelope, so a
  wrong guess never derives a real key. The wipe routine itself is a new
  shared `FactoryWipe` module, also used by the existing manual Factory
  Reset. Configured via a small popup (Enable/Disable/Reset), with
  validation that the duress password differs from the real one. See
  `docs/duress-password.md`.
- **Security:** optional auto-lock (Settings > Security > Auto-Lock).
  Pick an idle timeout — 30 min, 1h, 4h, 8h, 12h, or Disabled (the
  default) — and once the device has been idle that long it reboots
  back to the at-rest password screen, discarding the decrypted identity
  from RAM until the real password is re-entered.
- **Messaging:** anti-spam proof-of-work stamps (`LXStamper`), for peers
  that require one before accepting a message. Stamps generate in the
  background on their own task so sending stays responsive while the
  device grinds the proof of work; a confirm overlay asks first if the
  required cost is high enough to take a while. Settings > Messaging
  gets a stamp cost ceiling editor — stamps above it prompt instead of
  generating silently. See `docs/lxmf-stamps.md`.
- **Radio:** new Diagnostics screen (Settings > Radio > Diagnostics),
  separate from the existing Frequency/SF/BW/CR/TX Power editor (now
  under Settings > Radio > Config). Three pages, cycled with `;`/`.`:
  Live (TX/RX throughput graph, RSSI/SNR/noise floor, airtime usage,
  chip/link status, time since last RX/TX), Counters (packet/failure/
  CRC-fail/queue-drop counts and decoded radio device errors, clearable),
  and Network (current PHY config, Reticulum path/link counts, last
  announce time, known peers, LXMF outbox depth, heap/loop health).
  Backed by new packet/error counters on SX1262/LoRaInterface and a new
  reusable `StatGraph` widget.
- **GPS:** system clock now syncs to UTC directly from the GPS RMC
  sentence instead of relying on a user-picked timezone. For local time
  display, `GPSManager` checks the GPS fix's coordinates against a new
  `TimeZoneDB` table of real POSIX TZ rules covering major regions, so
  DST transitions land on the correct date; positions outside the table
  fall back to a longitude/season estimate. Settings > Time adds a
  manual override (toggle + UTC offset field) for when the automatic
  estimate is wrong. The status bar clock now has its own permanent
  spot instead of alternating with battery percentage, and dims until
  GPS confirms a real satellite fix.

### Fixed

- **LoRa:** the radio had no collision avoidance at all — it keyed up
  immediately whenever it wasn't itself mid-TX/RX, with no notion of
  another node transmitting on the same channel. On a shared
  half-duplex channel with more than one active node, this was a direct
  cause of delayed/dropped/reordered delivery. Added listen-before-talk:
  `SX1262::dcd()` detects an in-progress carrier/header via the modem's
  PREAMBLE_DET/HEADER_DET IRQ flags, and `LoRaInterface` now queues
  instead of transmitting into a busy channel, draining the queue once
  the channel clears after a randomized contention window sized from
  the radio's own symbol time.
- **LoRa:** fixed a duplicate-detection bug in microReticulum's
  packet-hashlist eviction — it's a `std::set<Bytes>` ordered by hash
  value, but eviction was erasing from `.begin()` as if that were
  insertion order, so culling discarded whichever hashes were
  numerically smallest instead of the actually-oldest ones. Under
  sustained traffic this silently undermined duplicate detection. Now
  tracks real insertion order separately
  (`cryptspeak/microReticulum@3481921`).
- **Routing:** routing and reply targets broke across every reboot —
  the message store was initialized before the identity was loaded, and
  saved conversation directories were keyed by a truncated hash too
  short to address a reply back to. Fixed by initializing the message
  store with the loaded identity before refresh, and resolving saved
  conversation directories to full peer hashes for reply targets.
- **LXMF:** sends to a peer with no known path were gated on a flat
  10s retry timer even after the path had already arrived, so the best
  case was a full 10-20s wait. `sendDirect()` now re-checks
  `recall()`/`has_path()` every queue-drain pass (10Hz) and only
  throttles the actual network-touching `request_path()`/
  `ensureOutboundLink()` calls to the 10s interval.
- **LXMF:** replaced the reactive, one-shot path nudge for single-hop
  peers (raced the reply it was meant to fix, and missed any relayed
  message) with a durable per-peer preferred-interface record in
  `AnnounceManager`, persisted across reboots alongside saved contacts.
- **LXMF:** peer names could go stale or vanish after a reboot, or even
  mid-session, because names were only ever learned from
  `AnnounceManager`'s own rate-limited, debounced announce handler. Name
  lookups now fall back to RNS's own `Identity::recall_app_data()`
  cache — populated for every announce Transport validates, independent
  of `AnnounceManager` — and opportunistically backfill the on-disk name
  cache when they resolve a name from it.
- **LXMF:** a peer name change never marked the UI dirty, so
  `MessagesScreen` wouldn't pick up an updated name while idle on
  screen; a name-change callback now calls `ui.markContentDirty()`
  directly.
- **microReticulum:** picked up two upstream fixes via dependency pin
  bumps: the previously-stubbed Link retransmission watchdog plus two
  resource-transfer bugs (windowed part requests, and outbound/inbound
  resource slots never freeing after a transfer concludes;
  `a71a676`); and a `RequestReceipt` constructor that threw
  `new std::invalid_argument(...)` (a pointer, not a value), which
  silently slipped past any upstream `catch (const std::exception&)`
  and leaked the heap-allocated exception (`72bdc81`).

### Changed

- Settings > Wi-Fi > Mode now opens a popup listing OFF/AP/STA with the
  active mode marked, instead of silently cycling to the next mode on
  every Enter press.
- Unified every small fixed-choice setting (Audio, Time Source,
  Auto-discover LAN, Theme input mode, Auto-Lock, WiFi Mode, Duress
  Password) onto one shared `OptionPopup` widget, replacing a mix of
  silent cycle-on-Enter toggles, a dedicated full-screen page, and an
  ad-hoc popup. Full-screen lists are now reserved for genuinely
  open-ended choices (WiFi scan, TCP connections, Theme presets). Also
  cleaned up unclear labels (Radio Config's SF/BW/CR, SD Card's
  "Initialize Standalone", TCP's "+ Add Connection", Display's bare
  "Name:") and moved SD Card to sit with About/Factory Reset instead of
  next to frequently-tuned settings. No stored values, validation, or
  applied behavior changed — UI/IA only.
- Duress Password moved from a dedicated settings page into the same
  popup style, with Enable/Disable/Reset rows.

### Removed

- **LXMF propagation node (offline messaging via PN) client**, added
  earlier in this cycle and fully removed before release: PN sync kept
  crashing and destabilizing routing on real hardware, and chasing it
  down kept colliding with the parts of this firmware that need to be
  solid — delivery, routing, the radio link. `PropagationClient` is
  deleted along with every call site, Settings UI screen, and config
  field that existed only to support it. Direct LXMF delivery, anti-spam
  stamps, routing/interface stickiness, and announce handling are
  unaffected — a message either reaches its recipient directly now, or
  it fails, same as before PN existed. `docs/propagation-nodes.md` kept
  as a short note on why this was dropped.
- Dead radio-preset code on the Home screen, and the status card it fed.

### Build

- `microReticulum` dependency now points at a personal fork (pinned
  commit) so the stamp/field support `LXStamper` needs, plus the
  reliability fixes above, can land as real commits there instead of
  local patches.
- The packaged factory `.bin` now lands in the `.pio` build dir instead
  of the project root.

### Docs

- New `docs/duress-password.md` and `docs/lxmf-stamps.md`, plus
  `docs/propagation-nodes.md` explaining why propagation-node support
  was attempted and then removed. Cross-linked from the boot sequence,
  threat model, encryption overview, and UI framework docs.
- Noted a one-time UX side effect for installs migrating from upstream
  ratspeak/rsCardputer configs: the old boot-time timezone picker is
  gone (superseded by GPS time sync), so first boot after migrating
  re-runs LoRa region/frequency setup via the new `RadioSetupScreen`
  instead of inheriting it from a timezone pick.

Full diff: https://github.com/cryptspeak/csCardputer/compare/v0.0.5...v0.1.0

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

Full diff: https://github.com/cryptspeak/csCardputer/compare/v0.0.4...v0.0.5

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

Full diff: https://github.com/cryptspeak/csCardputer/compare/v0.0.3...v0.0.4

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

Full diff: https://github.com/cryptspeak/csCardputer/compare/v0.0.2...v0.0.3

## v0.0.2

- Added the [Web Flasher](https://cryptspeak.github.io/webFlasher/)
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

Full release notes: https://github.com/cryptspeak/csCardputer/releases/tag/v0.0.1
