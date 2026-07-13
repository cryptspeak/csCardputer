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
after `readString()`. Adding a new encrypted domain is purely a
caller-side change (`AnnounceManager`, `UserConfig`, ...) — neither
storage class needs to change.

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
and `UserConfig`'s NVS path both do this — see
[encryption-contacts-settings.md](encryption-contacts-settings.md#nvs-binary-safety).

### NVS key length

`Preferences::putBool()`/`putBytes()`/etc. silently refuse any key
longer than 15 characters (`NVS_KEY_NAME_MAX_SIZE - 1`) — the call
returns failure and nothing is stored, with no exception or crash to
surface the mistake. A migration-complete flag written under an
over-length key therefore never persists, and reads back as its default
every boot, silently re-running that migration forever instead of once.

Two flags hit this: `AnnounceManager`'s original contacts-filename
migration flag and `MessageStore`'s original conversation-directory
migration flag were both over 15 characters and never actually persisted
until shortened. When adding a new one-time-migration flag, keep the key
at 15 characters or under and verify it round-trips (`putBool()` returns
`true`, and a re-read after a fresh boot reflects the write) rather than
assuming a `Preferences` call that "looks right" actually stored anything.

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

## Message file timestamps

Message content is encrypted, but LittleFS and FAT both stamp every file
with the real wall-clock time it was written, independent of anything
the application does — that's filesystem metadata, readable from a raw
flash/SD dump without decrypting a single message. Combined with the
per-message file layout described in [lxmf-messaging.md](lxmf-messaging.md),
an unscrubbed timestamp would let an attacker reconstruct exactly when
each message arrived, even though the content itself is opaque.

`FlashStore::scrubTimestamp()` / `SDStore::scrubTimestamp()` reset a
file's mtime/atime to a fixed epoch value right after it's written:

```cpp
bool FlashStore::scrubTimestamp(const char* path) {
    if (!_ready) return false;
    FSLock lock(_mutex);
    String full = String(FLASH_BASE_PATH) + path;
    struct utimbuf times = {0, 0};  // epoch — same value for every file
    return utime(full.c_str(), &times) == 0;
}
```

This goes around the Arduino File API rather than through it — `File`
exposes `getLastWrite()` but no setter. Both `esp_littlefs` and FATFS
register a `utime()` VFS handler under the hood, so a raw POSIX
`utime()` call against the mounted path (`/littlefs/...` for flash,
`/sd/...` for SD — the mountpoints `FlashStore`/`SDStore` register
internally) reaches it directly.

Three call sites cover every place a message file is written:

- `WriteQueue::processJob()` — after every `writeDirect()` for a new
  message, on both backends.
- `MessageStore::updateMessageStatusByCounter()` — after rewriting a
  message file in place for a delivery/read-receipt status change,
  which would otherwise leave a fresh real-time mtime behind.
- `MessageStore::scrubExistingMessageTimestamps()` — a one-time,
  NVS-flagged migration (flag `msg_ts_scrubbed`) that walks every
  existing message file on both backends and scrubs it, for files
  written before this existed.

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
