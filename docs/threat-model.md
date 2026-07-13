# Threat Model

## In scope: physical access while powered off

This protects against an attacker who **physically obtains the device**
(or extracts its SD card / dumps its NVS or flash partition) while it is
**powered off**. Without the password, they cannot read:

- The Reticulum identity (and therefore cannot impersonate the device or
  decrypt anything that was encrypted to it over the network either,
  though that's Reticulum's own crypto doing the work, not this layer).
- Message history — content, and which peers this device has ever
  exchanged messages with (see "public material broadcast over the air"
  below).
- Saved contact names, and which hashes were saved as contacts (same).
- Device settings — WiFi passwords, TCP hub addresses, radio config,
  display name.
- Any peer's announced display name recoverable from microReticulum's own
  `known_destinations` table — populated for *every* validated announce,
  not just saved contacts (see [encryption-known-destinations.md](encryption-known-destinations.md)).
- **When** each message was sent or received. Message file content is
  encrypted, but the underlying filesystem stamps every file with its
  real wall-clock write time regardless — `FlashStore::scrubTimestamp()`/
  `SDStore::scrubTimestamp()` reset that to a fixed, non-informative
  value on every write, so a raw dump can't reconstruct a message
  timeline from file metadata alone. See [storage-layer.md](storage-layer.md#message-file-timestamps).

## Coercion: the optional duress password

Everything above assumes an attacker who only has the *device*, not the
*user*. That doesn't hold for a border search, a robbery, or an abuse
situation, where someone can simply force the real password out of the
user. For that case there's an opt-in duress password (off by default):
entered at the unlock screen instead of the real one, it wipes the device
instead of unlocking it, then reboots into the same first-boot setup
screen a brand-new device shows — no special "wiped" state visible to
whoever is watching. See [duress-password.md](duress-password.md) for the
full mechanism, including why a plain reboot was chosen over a more
elaborate cover story: against an adversary who has actually read this
firmware's source, neither option hides that a wipe happened, so the
extra complexity bought nothing against that adversary while a plain
reboot was still the most mundane outcome against the realistic one
(someone coercing the password in the moment, not auditing the codebase).

## Explicitly out of scope

- **The device while unlocked and running.** Once the password has been
  entered, the identity's private key lives in RAM in plaintext — that's
  unavoidable, Reticulum needs it there to function. This is an at-rest
  protection, not a runtime sandbox. An attacker with code execution on a
  running device gets everything, same as any other embedded device.
- **Reticulum/LXMF packets on the wire.** LoRa, WiFi, and TCP traffic use
  Reticulum's own protocol-level encryption (Ed25519 identities,
  Reticulum's ratchet), completely independent of this at-rest layer.
  Nothing here changes the on-air security model.
- **Public material broadcast over the air.** A destination hash existing,
  or having been announced and seen by *some* device on the network, is
  not secret — that's how Reticulum discovery works, and no amount of
  local at-rest encryption changes it.

  This does **not** extend to which of those hashes *this specific
  device* has message history with or has saved as a contact — a much
  stronger claim than "a hash was announced." That's why conversation
  directories and contact files are named with `dirTokenForPeer()` /
  `contactFileToken()` (identity-keyed blind indices — see
  [encryption-contacts-settings.md](encryption-contacts-settings.md))
  rather than the hash itself: a directory listing alone must not answer
  "who has this device talked to," even without decrypting anything.
- **The color theme.** `theme.json` carries no sensitive information and is
  deliberately stored in plaintext outside the encrypted settings blob — the
  boot/password-gate screens need a theme before any identity is unlocked.
  See [theme-config.md](theme-config.md) for why a tampered copy of this
  file is bounded to "looks wrong," never a crash or an attack surface.
- **Weak passwords.** PBKDF2 with 65,536 iterations raises the cost per
  guess, but it cannot turn `password123` into a strong secret. This is a
  cost multiplier, not a substitute for picking a real passphrase.
- **Password recovery.** There is no recovery code and no backdoor.
  Losing the password means losing access to everything it protects —
  by design; any recovery mechanism would also be an attacker's way in.

## Why lockout doesn't persist across power cycles

Ten consecutive wrong-password attempts halt the boot for that power
cycle, but the counter resets on the next power-on rather than being
written to persistent storage. This is intentional: a persistent lockout
counter would itself need to be tamper-resistant (otherwise an attacker
just resets it the same way they'd bypass it), and getting that wrong
risks the worse failure mode — a legitimate user permanently locked out
of their own device by a glitch or partial write. The actual brute-force
defense is the PBKDF2 cost (~2-3 seconds per attempt scales to a long
time over millions of guesses), not the attempt counter; the counter
just adds friction within a single session.

## Why contacts, settings, and known_destinations are all in scope

The device is a single trust boundary: "physical access while off"
either holds for all persisted data or it doesn't meaningfully hold at
all. Contacts and settings carry real information on their own — every
name privately attached to a contact, plus WiFi network names/passwords
and any configured TCP relay hosts.

Encrypting the app's own contacts/name-cache files isn't sufficient by
itself, either: microReticulum keeps a second, independent record of
every peer's announced display name in its `known_destinations` table,
flushed to storage by the library itself
(`Identity::save_known_destinations()`), outside `ContactsEncryption`
entirely. It's populated for every destination whose announce has ever
been validated, whether or not that peer was explicitly saved as a
contact — in practice, every peer with any message history at all, since
receiving their first announce is how their identity key was learned in
the first place. Left unencrypted, this file alone would be sufficient
to reconstruct "who this device has talked to" from a raw flash dump,
even with zero saved contacts. See
[encryption-known-destinations.md](encryption-known-destinations.md).
