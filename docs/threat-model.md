# Threat Model

## In scope: physical access while powered off

This protects against an attacker who **physically obtains the device**
(or extracts its SD card / dumps its NVS or flash partition) while it is
**powered off**. Without the password, they cannot read:

- The Reticulum identity (and therefore cannot impersonate the device or
  decrypt anything that was encrypted to it over the network either,
  though that's Reticulum's own crypto doing the work, not this layer).
- Message history.
- Saved contact names (as of this change).
- Device settings — WiFi passwords, TCP hub addresses, radio config,
  display name (as of this change).

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
- **Public material.** Announce-derived contact hashes and destination
  hashes are intentionally not secret — they're broadcast over the air as
  part of how Reticulum's discovery works. Encrypting a contact's *name*
  protects something you added privately; it can't and doesn't try to
  hide the fact that some hash exists or was seen.
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

## Why contacts and settings needed closing

Identity and messages were already covered before this branch. Contacts
and settings being unencrypted meant an attacker with disk access learned
strictly less than with messages exposed, but still got real information
for free: every name you'd privately attached to a contact, plus your
WiFi network names/passwords and any TCP relay hosts you'd configured —
all without needing the password at all. Bringing them into the same
encrypted-at-rest model removes that gap without changing the overall
threat model — it was already "physical access while off," this change
just makes that threat model actually hold for *all* persisted data
instead of two out of four categories.
