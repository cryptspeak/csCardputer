# rsCardputer-CE Documentation

This folder documents how the firmware works, with a focus on the at-rest
encryption system (the "Cryptspeak" feature set) since that is the part
most likely to need careful review.

## Encryption

| Doc | Covers |
|---|---|
| [encryption-overview.md](encryption-overview.md) | The big picture: what's protected, what isn't, the shared crypto engine, primitives used |
| [encryption-identity.md](encryption-identity.md) | Password → key derivation, identity wrap/unwrap, the `IdentityCrypto` module |
| [encryption-messages.md](encryption-messages.md) | At-rest encryption of LXMF message files, the `MessageEncryption` module |
| [encryption-contacts-settings.md](encryption-contacts-settings.md) | At-rest encryption of contacts, the name cache, and device settings — `ContactsEncryption` / `SettingsEncryption` |
| [boot-sequence.md](boot-sequence.md) | The full boot flow: identity probe, password gate, legacy migration, encryption wiring order |
| [duress-password.md](duress-password.md) | The optional second password that wipes the device instead of unlocking it — `Duress` / `FactoryWipe` modules, Settings UI |
| [threat-model.md](threat-model.md) | What this defends against, what it explicitly does not, and why |

## Firmware

| Doc | Covers |
|---|---|
| [firmware-architecture.md](firmware-architecture.md) | Directory layout, major modules, how they fit together |
| [storage-layer.md](storage-layer.md) | `FlashStore` / `SDStore` / `WriteQueue` — the storage tiers everything else is built on |
| [theme-config.md](theme-config.md) | Custom color themes — why `theme.json` is intentionally unencrypted, and why a malicious copy of it can't do anything beyond look ugly |

## Reticulum / LXMF core

| Doc | Covers |
|---|---|
| [reticulum-integration.md](reticulum-integration.md) | `ReticulumManager` — endpoint-only config, the LittleFS bridge, the background persist task, and announce-flood defense layer 1 |
| [lxmf-messaging.md](lxmf-messaging.md) | `LXMFManager` + `MessageStore` — send/receive flow, delivery proofs/retries, on-disk message format and capacity limits |
| [announce-discovery.md](announce-discovery.md) | `AnnounceManager` — announce-flood defense layers 2-3, app_data name parsing, the node table, contacts vs. name cache |
| [lxmf-stamps.md](lxmf-stamps.md) | Anti-spam proof-of-work stamps — `LXStamper`'s streaming-SHA256 feasibility trick, the wire format, background generation, the cost ceiling |
| [propagation-nodes.md](propagation-nodes.md) | Offline messaging via store-and-forward PNs — `PropagationClient`'s discovery/outbound-fallback/inbound-sync, client-only scope |

## Network & hardware

| Doc | Covers |
|---|---|
| [network-interfaces.md](network-interfaces.md) | LoRa/WiFi/TCP/AutoInterface/BLE interfaces, the SX1262 driver, shared-SPI arbitration |
| [hardware-platform.md](hardware-platform.md) | GPS, keyboard/hotkeys, power management, audio, pin reference |
| [ui-framework.md](ui-framework.md) | The `Screen`/`UIManager` framework, dirty-flag rendering, input routing, widgets, screen index |

## Where to start

If you only read one thing, read [encryption-overview.md](encryption-overview.md) —
it has the full picture in one page and links into the detail docs for each
data category.
