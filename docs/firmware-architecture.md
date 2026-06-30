# Firmware Architecture

csCardputer is a PlatformIO/Arduino firmware for the M5Stack Cardputer
Adv (ESP32-S3). It's a single standalone image — there's no host-side
companion app; everything runs on-device. `src/main.cpp` is the entry
point (`setup()`/`loop()`), wiring together the modules below.

## Directory map

```
src/
├── main.cpp            Boot sequence, main loop, screen wiring, hotkeys
├── config/              Compile-time constants (Config.h, BoardConfig.h) + UserConfig (runtime settings)
├── reticulum/            Reticulum/LXMF integration layer
│   ├── ReticulumManager   Identity lifecycle, transport bring-up
│   ├── IdentityCrypto     At-rest encryption of the identity (see docs/encryption-identity.md)
│   ├── LXMFManager        LXMF message send/receive glue
│   └── AnnounceManager     Contact discovery, persistence, name cache
├── storage/              Filesystem abstractions + at-rest crypto
│   ├── FlashStore / SDStore   Thin wrappers over LittleFS / SD with atomic writes
│   ├── WriteQueue              Async write queue for message saves (non-blocking)
│   ├── MessageStore             LXMF message persistence + pagination
│   ├── MessageEncryption         At-rest encryption: messages
│   ├── ContactsEncryption         At-rest encryption: contacts (added in this branch)
│   ├── SettingsEncryption          At-rest encryption: settings (added in this branch)
│   └── AtRestCrypto                 Shared envelope engine behind the three above
├── transport/            Network interfaces: LoRa, WiFi (AP/STA), TCP client, BLE stub, AutoInterface
├── radio/                SX1262 LoRa radio driver
├── hal/                  Hardware glue: shared SPI bus arbitration, GPS/NMEA
├── power/                Display dim/off timeout, brightness management
├── audio/                Notification sounds
├── input/                Keyboard driver, hotkey registration
└── ui/                   Screen framework, status/tab bars, theming
    └── screens/           Individual screens (Boot, Password, Home, Messages, Settings, Nodes, ...)
```

## Key relationships

- **`ReticulumManager`** owns the `RNS::Identity` and the Reticulum
  transport. It's the single source of truth for "is the identity
  unlocked yet" — everything that needs at-rest encryption (`MessageStore`,
  `AnnounceManager`, `UserConfig`) takes a `const RNS::Identity*` pointer
  from it via a `setIdentity()` call once it's available. See
  [boot-sequence.md](boot-sequence.md) for the exact ordering and
  [reticulum-integration.md](reticulum-integration.md) for everything
  else `ReticulumManager` does (endpoint config, announce-flood defense,
  the background persist task).
- **`LXMFManager`** is the send/receive state machine sitting on top of
  `ReticulumManager`'s destination; **`AnnounceManager`** is the
  discovery/contacts side. See [lxmf-messaging.md](lxmf-messaging.md)
  and [announce-discovery.md](announce-discovery.md).
- **`FlashStore`/`SDStore`** are the only things that touch the
  filesystem directly. Every higher-level module (`MessageStore`,
  `AnnounceManager`, `UserConfig`) reads/writes through one of these two,
  encrypting/decrypting the String payload before/after the call. See
  [storage-layer.md](storage-layer.md).
- **`AtRestCrypto`** is the one place that implements AES-256-CTR+HMAC
  envelope encryption; `MessageEncryption`, `ContactsEncryption`, and
  `SettingsEncryption` are thin per-domain wrappers around it.
  `IdentityCrypto` is separate because it derives its key from a
  password via PBKDF2 instead of from the identity via HKDF — see
  [encryption-overview.md](encryption-overview.md).
- **`transport/` + `radio/`** implement the actual `RNS::Interface`s
  (LoRa, WiFi, TCP, AutoInterface, the BLE stub) and the SX1262 driver
  underneath LoRa. See [network-interfaces.md](network-interfaces.md).
- **`hal/`, `input/`, `power/`, `audio/`** are board-level glue with no
  Reticulum or storage knowledge of their own. See
  [hardware-platform.md](hardware-platform.md).
- **UI screens** (`src/ui/screens/`) are largely independent and talk to
  the managers above through narrow setter/getter APIs
  (`setUserConfig()`, `setIdentityHash()`, etc.) rather than reaching into
  storage directly. See [ui-framework.md](ui-framework.md) for the
  framework they're built on.

## Build

Single PlatformIO environment, `standalone_915` (915 MHz region):

```bash
pio run -e standalone_915
pio run -e standalone_915 -t upload
```
