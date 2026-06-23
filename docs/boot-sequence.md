# Boot Sequence

Source: `setup()` in [`src/main.cpp`](../src/main.cpp).

This walks through `setup()` in order, focusing on where encryption keys
become available and exactly when each data category gets wired up. Line
numbers will drift as the file changes; look for the named calls instead.

```
1. M5Cardputer.begin() / display / keyboard / hotkeys init
   (no encryption involved — pure hardware bring-up)

2. flash.begin()                          — mount LittleFS
3. Boot-loop detection                    — NVS "ratcom"/"bootc" counter (unrelated namespace)
4. radio.begin()                          — SX1262 bring-up
5. sdStore.begin()                        — mount SD card, ensure base dirs

   ── Identity-at-rest gate ──────────────────────────────────────────────
6. rns.probeIdentityState()
     → NONE              : no identity anywhere, first boot
     → ENCRYPTED         : RID1-magic blob found, needs password to open
     → LEGACY_PLAINTEXT  : pre-Crypto-Edition plaintext identity found

7. runPasswordGate(state)
     SETUP   path: type + confirm a new password (≥6 chars)
     UNLOCK  path: type once, up to 10 attempts, IdentityCrypto::unwrap()
                   verifies the MAC before any decryption is attempted
     → returns { password, unlockedKey }

8. rns.setPassword(password)
   rns.preloadIdentityKey(unlockedKey)     — if we already unwrapped externally

9. rns.begin(&radio, &flash)
     - Adopts the preloaded key, or loads/creates the identity
     - rns.identity() is now valid and holds the private key in RAM
   ── Everything below this line can derive at-rest keys from the identity ──

10. messageStore.begin(&flash, &sdStore)
    messageStore.setIdentity(&rns.identity())   ← message encryption enabled

11. if (rns.legacyIdentityLoaded()):
        runLegacyIdentityMigration()
          - identity already re-wrapped & encrypted by saveIdentityToAll()
          - user is asked: migrate messages too? (opt-in, may take a while)
          - if yes: messageStore.migratePlaintextMessages() with progress UI

12. announceManager = new AnnounceManager(...)
    announceManager->setIdentity(&rns.identity())   ← contacts encryption enabled
    announceManager->loadContacts()
    announceManager->loadNameCache()
    if (announceManager->encryptionEnabled()):
        announceManager->saveContacts()     ← forces immediate re-encrypt of
        announceManager->saveNameCache()      any pre-upgrade plaintext files

13. userConfig.setIdentity(&rns.identity())          ← settings encryption enabled
    userConfig.load(sdStore, flash)
    if (userConfig.encryptionEnabled()):
        userConfig.save(sdStore, flash)     ← forces immediate re-encrypt of
                                               a pre-upgrade plaintext settings file

14. ... WiFi/AutoInterface bring-up, GPS, power/audio settings applied,
        home screen shown
```

## Why contacts/settings migration is unconditional but message migration isn't

Message history can be arbitrarily large — re-encrypting thousands of
files at boot would stall startup, so that migration is opt-in with a
progress screen (step 11). Contacts are capped at 50 entries and settings
is a single file; re-encrypting them is sub-millisecond, so steps 12 and
13 just do it unconditionally with no user-visible delay or prompt. There
is no scenario where waiting for confirmation would be worth the UX cost.

## Why this ordering is safe

The identity must exist in plaintext in RAM before *any* domain key can
be derived (every domain's key traces back to
`identity.encryptionPrivateKey()`). Steps 10, 12, and 13 are all placed
after step 9 specifically because that's the earliest point the identity
is guaranteed valid. Nothing earlier in `setup()` reads contacts or
settings — the boot screen, keyboard, hotkeys, radio defaults, and SD
mount all use hardcoded values or don't need persisted state at all, so
there was no pre-existing code path that would have broken by requiring
identity-first ordering.

## Password lifetime

The plaintext password string only exists in RAM during steps 7-9; it is
explicitly zeroed (`IdentityCrypto::secureZero`) once `rns.begin()` has
adopted the identity. From that point on, only the *identity's private
key* lives in RAM (which Reticulum itself requires for normal operation —
that's not something this firmware can avoid), not the password that
unlocked it.
