<div align="center">

# rsCardputer — Crypto Edition

**Standalone Ratspeak firmware for the M5Stack Cardputer Adv, with at-rest-encryption for your identity and messages.**

[![Status](https://img.shields.io/badge/status-beta-yellow.svg)](#status)
[![Model](https://img.shields.io/badge/model-Cardputer%20Adv-success.svg)](https://docs.m5stack.com/en/core/Cardputer-Adv)
[![Encryption](https://img.shields.io/badge/at--rest-AES--256--CTR%20%2B%20HMAC-success.svg)](#crypto-edition--what-changed)
[![License](https://img.shields.io/badge/license-mixed-blue.svg)](#license)

[Ratspeak](https://github.com/ratspeak/Ratspeak) |
[rsReticulum](https://github.com/ratspeak/rsReticulum) |
[rsLXMF](https://github.com/ratspeak/rsLXMF) |
[Downloads](https://ratspeak.org/download.html)

</div>

---

rsCardputer-CE is a standalone Reticulum/LXMF firmware image for the M5Stack Cardputer Adv. This repository currently builds only the standalone image, and the current encryption design is implemented inside that firmware.

On boot, the device starts the standalone Reticulum/LXMF messenger interface.

## Contents

- [Crypto Edition — what changed](#crypto-edition--what-changed)
- [Hardware](#hardware)
- [Flashing](#flashing)
- [Radio Presets](#radio-presets)
- [Standalone Mode](#standalone-mode)
- [Build From Source](#build-from-source)
- [License](#license)

## Crypto Edition — what changed

This build adds **mandatory at-rest encryption** for the Reticulum identity
and stored messages. The Reticulum and LXMF protocols on the wire are
**unchanged** — this is purely about protecting what lives on the device's
flash, NVS, and SD card after the device is powered off.

### What's protected

| Asset | Before | Now |
|---|---|---|
| Identity private key (flash, NVS, SD) | Plaintext | AES-256-CTR + HMAC-SHA256, password-wrapped |
| Message bodies on disk (LXMF JSON) | Plaintext | AES-256-CTR + HMAC-SHA256, identity-derived |
| Announces / known destinations | Plaintext (public) | Unchanged (public by design) |
| Reticulum packets on the air | Reticulum protocol crypto | Unchanged |

### Boot flow

```
Power on
   │
   ▼
┌──────────────────────────────────────────┐
│  Identity probe                          │
│   • Encrypted blob found  → UNLOCK       │
│   • Plaintext identity    → SETUP + MIGRATE
│   • Nothing               → SETUP        │
└──────────────────────────────────────────┘
   │
   ▼
┌──────────────────────────────────────────┐
│  Password screen (VeraCrypt-style)       │
│   SETUP:  type + confirm (min 6 chars)   │
│   UNLOCK: type once, up to 10 attempts   │
│            per power cycle               │
└──────────────────────────────────────────┘
   │
   ▼
  Reticulum starts → home screen
```

If a legacy plaintext identity is detected, after the password is set the
device prompts you to choose how much to migrate:

- **Identity only** *(fast)* — re-wraps the identity key on every tier and
  removes the plaintext copy. Existing messages stay readable but plaintext
  on disk.
- **Identity + messages** *(slow)* — additionally walks every stored
  conversation and re-encrypts each message file in place. May take a
  while with many messages; progress is shown.

### Crypto in plain English

- **Password → key**: PBKDF2-HMAC-SHA256, 65,536 iterations, 16-byte
  random salt per device. Takes a couple of seconds on the ESP32-S3 — that
  cost is also paid by anyone trying to brute-force your password.
- **Identity wrap**: the derived KEK is HKDF-split into an AES-256-CTR key
  and an HMAC-SHA256 key. The identity blob is encrypted, then MACed
  (encrypt-then-MAC). On unlock, the MAC is checked **before** any
  decrypt with a constant-time comparison — wrong password, tampered file,
  or bit-rot all fail without ever producing plaintext.
- **Message wrap**: at-rest message keys are derived from your identity's
  X25519 private key via HKDF-SHA256, with the identity hash used as salt.
  Each file gets a fresh random 16-byte IV, so identical messages don't
  produce identical ciphertext (no ECB-style structure leakage).
- **No global key reuse, no malleability**: the MAC covers
  `magic | version | IV | ciphertext`, so any tampering, truncation, or
  identity mismatch is rejected.
- **Lockout**: ten consecutive wrong passwords in one power cycle halts
  the boot; power the device off and on to try again. No data is wiped on
  lockout — losing access does not mean losing data.
- **Password loss = data loss**: there is no recovery code, no backdoor,
  and the Ratspeak team cannot recover it for you. Pick something you'll
  remember.

### What this does NOT do

- It does not change anything on the LoRa air or over WiFi/TCP — incoming
  and outgoing Reticulum/LXMF packets use the protocol's own crypto.
- It does not encrypt public material: announce-derived contacts,
  destination tables, your own announce app-data.
- It is not a substitute for full-disk encryption of a stolen SD card if
  the user picks a weak password. PBKDF2 raises the cost per attempt but
  cannot rescue `password123`.

### Threat model

This protects against an attacker who **physically obtains** the device
(or its SD card / NVS dump) while it is powered off. They cannot read
the identity or message history without the password. It does **not**
protect against an attacker who has the device unlocked and running.

## Hardware

The supported Cardputer Adv has:

- ESP32-S3FN8
- 8 MB internal flash
- Built-in keyboard and 240x135 display
- microSD support (32GB max), optional but recommended
- **SX1262 LoRa radio***

*LoRa capabilities are only possible with the Cardputer Adv LoRa cap (module), which they usually sell separately. Without it, the Cardputer will only have WiFi/BLE capabilities.

## Flashing

This repository builds only the standalone firmware. To flash the device, build and upload manually with PlatformIO as described in [Build From Source](#build-from-source).

## Radio Presets

| Preset | SF | BW | CR | TXP | Bitrate | Link budget |
|---|---|---|---|---|---|---|
| Short Turbo | 7 | 500 kHz | 4/5 | 14 dBm | 21.99 kbps | 140 dB |
| Short Fast | 7 | 250 kHz | 4/5 | 14 dBm | 10.84 kbps | 143 dB |
| Short Slow | 8 | 250 kHz | 4/5 | 14 dBm | 6.25 kbps | 145.5 dB |
| Medium Fast | 9 | 250 kHz | 4/5 | 17 dBm | 3.52 kbps | 148 dB |
| Medium Slow | 10 | 250 kHz | 4/5 | 17 dBm | 1.95 kbps | 150.5 dB |
| Long Turbo | 11 | 500 kHz | 4/8 | 22 dBm | 1.34 kbps | 150 dB |
| **Long Fast** *(default)* | **11** | **250 kHz** | **4/5** | **22 dBm** | **1.07 kbps** | **153 dB** |
| Long Moderate | 11 | 125 kHz | 4/8 | 22 dBm | 0.34 kbps | 156 dB |

Standalone mode exposes radio settings on-device.

The supported SX1262 cap is an 850-950 MHz radio target. 868 MHz and 915 MHz
are valid software profiles for that hardware range. 433 MHz requires 433 MHz
radio hardware. You are responsible for operating within your local laws.

## Standalone Mode

This firmware supports only standalone mode in its current form.

## Build From Source

To build the firmware from source, install the prerequisites:

- Python 3
- PlatformIO

Install the Python tooling:

```bash
python3 -m pip install platformio esptool
```

Build the standalone firmware with:

```bash
pio run -e standalone_915
```

Flash it manually with PlatformIO:

```bash
pio run -e standalone_915 -t upload
```

> Note: this repository currently requires manual PlatformIO build and upload; the Makefile workflow is not fully functional yet.

## License & Contribution

Ratspeak's Cardputer standalone application, launcher, partition tables, and packaging tools are licensed under the GNU Affero General Public License v3.0 or later. See [LICENSE](LICENSE).

The bundled RNode firmware under `vendor/rnode_firmware/` is licensed under
the GNU General Public License v3.0. See [LICENSE-RNODE](LICENSE-RNODE).a

Standalone mode uses a [custom fork](https://github.com/ratspeak/microReticulum) of microReticulum, which was originally written by [Chad Atterman](https://github.com/attermann).
