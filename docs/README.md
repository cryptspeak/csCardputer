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
| [threat-model.md](threat-model.md) | What this defends against, what it explicitly does not, and why |

## Firmware

| Doc | Covers |
|---|---|
| [firmware-architecture.md](firmware-architecture.md) | Directory layout, major modules, how they fit together |
| [storage-layer.md](storage-layer.md) | `FlashStore` / `SDStore` / `WriteQueue` — the storage tiers everything else is built on |
| [theme-config.md](theme-config.md) | Custom color themes — why `theme.json` is intentionally unencrypted, and why a malicious copy of it can't do anything beyond look ugly |

## Where to start

If you only read one thing, read [encryption-overview.md](encryption-overview.md) —
it has the full picture in one page and links into the detail docs for each
data category.
