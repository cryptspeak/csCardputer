# Storage Layer

Three tiers of physical storage are used, in priority order: **SD card**
(primary, when present), **flash/LittleFS** (always available, smaller),
and **NVS** (internal flash, wear-leveled, used only as a bulletproof
backup for small data — identity key, settings blob).

## `FlashStore` / `SDStore`

[`src/storage/FlashStore.{h,cpp}`](../src/storage/FlashStore.h),
[`src/storage/SDStore.{h,cpp}`](../src/storage/SDStore.h)

Thin wrappers over LittleFS and the SD library respectively, exposing the
same shape of API: `readString()`, `writeString()` (atomic — writes to a
`.tmp` file, then renames, keeping a `.bak` of the previous version),
`writeDirect()` (non-atomic fallback if the rename dance fails),
`ensureDir()`, `remove()`, `openDir()`.

**Neither of these classes knows anything about encryption.** They treat
every payload as an opaque byte buffer (an Arduino `String` used as a byte
container, not as text — see the note on embedded NUL bytes below).
Encryption is always applied by the caller before `writeString()` and
after `readString()`. This separation is why adding contacts/settings
encryption in this branch didn't require touching either of these
classes at all — only the callers (`AnnounceManager`, `UserConfig`)
changed.

### A note on `String` as a byte buffer

Several modules — `MessageEncryption`, `ContactsEncryption`,
`SettingsEncryption` — produce ciphertext that may contain embedded
`0x00` bytes (it's binary, not text), but pass it around as an Arduino
`String`. This works because `String` tracks its length explicitly and
`readString()`/`writeString()` on `FlashStore`/`SDStore` use that length
rather than scanning for a null terminator. The one place this *doesn't*
hold is `Preferences` (NVS) — `putString()`/`getString()` are
null-terminated under the hood, so any code writing a possibly-binary
blob to NVS must use `putBytes()`/`getBytes()` instead. `IdentityCrypto`
always did this correctly; `UserConfig`'s NVS path was fixed to do the
same as part of the settings-encryption work — see
[encryption-contacts-settings.md](encryption-contacts-settings.md) for
the before/after.

## `WriteQueue`

[`src/storage/WriteQueue.{h,cpp}`](../src/storage/WriteQueue.h)

A small FreeRTOS-task-backed async write queue, used **only by
`MessageStore`**. Message saves need to return in well under a
millisecond (they can be called from contexts where blocking on SD I/O
would be a problem), so writes are enqueued and flushed by a background
task instead of happening inline.

Contacts and settings do **not** use `WriteQueue` — both go through
direct, synchronous `writeString()` calls. This is intentional, not an
oversight: contact saves are already rate-limited/batched at a higher
level (`AnnounceManager`'s `CONTACT_SAVE_INTERVAL_MS` dirty-flag
mechanism), and settings saves are infrequent, explicit user actions
(changing a setting in the UI) rather than something on a hot path.

## Where encryption sits relative to these tiers

```
   caller (MessageStore / AnnounceManager / UserConfig)
        │
        ├─ encrypt (AtRestCrypto-backed domain module)
        │
        ▼
   FlashStore::writeString() / SDStore::writeString()   (opaque bytes, atomic)
        │
        ▼
   LittleFS / SD card
```

Reads run the same path in reverse: storage → caller → decrypt (or
pass through, if the buffer doesn't carry the domain's magic header,
meaning it's a legacy pre-encryption file).
