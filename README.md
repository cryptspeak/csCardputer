<p align="center">
  <img src="assets/cryptspeak.svg" alt="Cryptspeak" width="300" />
</p>

<p align="center">
  <b>Standalone LXMF messenger for the M5Stack Cardputer Adv</b>
</p>

<p align="center">
  <a href="#"><img src="https://img.shields.io/badge/status-beta-yellow.svg"></a>
  <a href="https://docs.m5stack.com/en/core/Cardputer-Adv"><img src="https://img.shields.io/badge/hardware-Cardputer%20Adv-success.svg"></a>
  <a href="docs/encryption-overview.md"><img src="https://img.shields.io/badge/at--rest-AES--256--CTR%20%2B%20HMAC-success.svg"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPLv3-blue.svg"></a>
</p>

---

Cryptspeak is a security-hardened fork of [ratspeak/rsCardputer](https://github.com/ratspeak/rsCardputer). It runs Reticulum and LXMF directly on the device — no companion app, no accounts, no central server. It communicates over LoRa radio and WiFi/TCP, whichever is available.

The key addition over the base firmware is **mandatory at-rest encryption**. Every sensitive asset the device writes to storage is encrypted before it touches flash, NVS, or SD card. If someone takes the device without the password, they get an encrypted brick.

**→ [Flash with the Web Flasher](https://cryptspeak.github.io/webFlasher/)** — Chrome or Edge, no toolchain needed.

---

## What it protects

**Powered off or locked:** everything on storage is ciphertext. An attacker with the hardware, the SD card, or a raw flash dump cannot read your identity key, messages, contacts, or device settings without the password.

**Powered on and unlocked:** the identity private key is in RAM in plaintext — Reticulum needs it there to function. This is an at-rest layer, not a runtime sandbox.

| Asset | Protection |
|---|---|
| Identity private key | AES-256-CTR + HMAC-SHA256, password-derived (PBKDF2) |
| Messages | AES-256-CTR + HMAC-SHA256, identity-derived (HKDF) |
| Contacts + name cache | AES-256-CTR + HMAC-SHA256, identity-derived (HKDF) |
| Device settings (WiFi, TCP hubs, radio config) | AES-256-CTR + HMAC-SHA256, identity-derived (HKDF) |
| Reticulum packets on the wire | Unchanged — Reticulum's own protocol crypto |

Each domain has a separate HKDF-derived key. A key for one domain cannot decrypt another.

**Password loss = data loss.** There is no recovery path and no backdoor — any recovery mechanism would also be an attacker's way in.

Ten consecutive wrong attempts halt the boot until the next power cycle. The brute-force defense is PBKDF2 cost (~2–3 s per attempt), not the attempt counter.

### Duress password

An optional second password (Settings → Security → Duress Password) that wipes the device instead of unlocking it — identity, messages, contacts, and settings across every storage tier — then reboots into the same setup screen a new device shows. Nothing indicates a wipe occurred. Useful for border crossings, robberies, or any situation where you might be forced to hand over the real password.

### Auto-lock

Reboots back to the password screen after a configurable idle timeout (30 min – 12 h; off by default). Settings → Security → Auto-Lock.

---

## Features

- **LoRa + WiFi/TCP** — both active simultaneously; LoRa requires the SX1262 cap
- **LXMF anti-spam stamps** — proof-of-work stamps computed in the background, configurable cost ceiling
- **Listen-before-talk** — carrier detection before every LoRa transmission
- **GPS time sync** — UTC from GPS with automatic timezone detection and manual override
- **Radio diagnostics** — live RSSI/SNR/throughput graphs, packet counters, Reticulum path stats
- **Identity verification** — show a peer's full 32-character LXMF address for comparison against a known-good hash
- **Color themes** — 9 presets plus custom hex/RGB
- **Web Flasher** — flash from Chrome/Edge over USB, no PlatformIO needed

---

## Hardware

Requires an **M5Stack Cardputer Adv**. LoRa requires the **SX1262 cap** (850–950 MHz). Without it, WiFi/TCP only.

| | |
|---|---|
| SoC | ESP32-S3FN8 |
| Flash | 8 MB internal |
| Display | 240×135 |
| Input | Built-in keyboard |
| Storage | microSD, 32 GB max (optional, recommended) |
| Radio | SX1262, 850–950 MHz (cap required) |

You are responsible for operating within your local frequency regulations.

---

## Flashing

The easiest path is the **[Web Flasher](https://cryptspeak.github.io/webFlasher/)** — Chrome or Edge with WebSerial, over USB. Flashes a pre-built release binary directly from the browser.

Release firmware is built by CI on every tagged commit. See [Releases](https://github.com/cryptspeak/csCardputer/releases).

### Build from source

```bash
python3 -m pip install platformio esptool
pio run -e standalone_915            # build
pio run -e standalone_915 -t upload  # flash
```

---

## Documentation

Full design docs in [`docs/`](docs/README.md):

| | |
|---|---|
| [threat-model.md](docs/threat-model.md) | What this defends against and what it doesn't |
| [encryption-overview.md](docs/encryption-overview.md) | Crypto engine, primitives, domain separation |
| [encryption-identity.md](docs/encryption-identity.md) | Password → key derivation, identity wrap/unwrap |
| [encryption-messages.md](docs/encryption-messages.md) | Message encryption |
| [encryption-contacts-settings.md](docs/encryption-contacts-settings.md) | Contacts and settings encryption |
| [boot-sequence.md](docs/boot-sequence.md) | Full boot flow |
| [duress-password.md](docs/duress-password.md) | Duress password design and rationale |
| [network-interfaces.md](docs/network-interfaces.md) | LoRa / WiFi / TCP interfaces |
| [lxmf-stamps.md](docs/lxmf-stamps.md) | Anti-spam proof-of-work stamps |
| [announce-discovery.md](docs/announce-discovery.md) | Peer discovery and name resolution |
| [firmware-architecture.md](docs/firmware-architecture.md) | Module layout |

---

## Status

Beta. The design uses established primitives rather than novel constructions, and the implementation is publicly documented. It has not yet received an independent manual security audit. If you want to review the implementation or perform an audit, contributions are welcome.

During development we found and fixed a signature verification bypass inherited from upstream microReticulum — `Identity::validate()` ignored the return value of Ed25519 verification and always reported success, effectively disabling authentication for signed packets. The fix has been merged upstream. ([issue](https://github.com/ratspeak/rsCardputer/issues/17) · [fix](https://github.com/ratspeak/microReticulum/pull/1))

---

## Credits & License

- **Base firmware** — [ratspeak/rsCardputer](https://github.com/ratspeak/rsCardputer)
- **Reticulum** — [cryptspeak/microReticulum](https://github.com/cryptspeak/microReticulum), forked from [ratspeak/microReticulum](https://github.com/ratspeak/microReticulum), itself from [attermann/microReticulum](https://github.com/attermann/microReticulum)
- **Crypto primitives** — [rweather/arduinolibs](https://github.com/rweather/arduinolibs) via [Chad Attermann's fork](https://github.com/attermann/Crypto), MIT licensed
- **Encryption design** — builds on [konsumer](https://github.com/konsumer)'s [arduino-rns-encrypted-store](https://github.com/konsumer/arduino-rns-encrypted-store) and [arduino-rns-password](https://github.com/konsumer/arduino-rns-password)
- **SX1262 driver** — adapted from [RNode Firmware CE](https://github.com/liberatedsystems/RNode_Firmware_CE), MIT licensed, © Sandeep Mistry, modifications by Mark Qvist & Jacob Eva

The standalone application, partition tables, and packaging tools are licensed under the **GNU Affero General Public License v3.0 or later**. See [LICENSE](LICENSE).

`src/radio/` and `lib/Crypto` carry their own MIT license terms — see attribution headers in those files.
