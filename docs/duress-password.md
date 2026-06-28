# Duress Password

Modules: [`src/security/Duress.{h,cpp}`](../src/security/Duress.h) (verifier
storage/check), [`src/storage/FactoryWipe.{h,cpp}`](../src/storage/FactoryWipe.h)
(shared wipe routine). Hooked into the boot unlock loop in
[`src/main.cpp`](../src/main.cpp) (`runPasswordGate()`). Configured from
Settings → Security (`src/ui/screens/SettingsScreen.cpp`).

## What it is

An optional second password. Entered at the boot unlock screen *instead of*
the real at-rest password, it wipes the device instead of unlocking it. This
exists for the coercion case the base encryption model can't cover: someone
forced to type their password in front of whoever is forcing them (border
search, robbery, abuse situation) can type the duress password instead and
the device erases itself.

Disabled by default — it's an opt-in toggle in Settings, not part of the
mandatory at-rest encryption flow.

## What happens after it's triggered

The device wipes everything (see "Wipe scope" below), then calls
`ESP.restart()` and boots straight into the same first-time-setup screen a
factory-fresh device shows. There is no "data wiped" message, no error
screen, nothing that marks this boot as different from a brand-new device.

This was a deliberate choice over a more elaborate cover story (a fake
crash screen was considered and rejected): the firmware is open source, so
an adversary who has actually read the code learns nothing less from a
plain reboot than from a fake panic screen — both are equally visible as
"only reachable via duress" in the source. Against the realistic adversary
(someone coercing the password in the moment, who has not pre-read several
thousand lines of C++), a plain reboot to setup is the most mundane, easiest
to explain outcome ("must have reset itself"). See
[threat-model.md](threat-model.md) for how this fits the rest of the
device's threat model.

## How the verifier works

There's no separate plaintext "duress enabled" flag. `Duress::isConfigured()`
is simply "does a verifier blob exist in NVS" — nothing to fall out of sync.

Setting it up wraps a fixed, public constant with the chosen duress password
using the exact same envelope `IdentityCrypto` already uses for the real
identity (PBKDF2-HMAC-SHA256 → AES-256-CTR + HMAC, `"RID1"` format — see
[encryption-identity.md](encryption-identity.md)):

```
Duress::setup(password)
    → IdentityCrypto::wrap(password, FIXED_PAYLOAD)  → blob
    → NVS: namespace "ratcom_duress", key "v"
```

Checking a candidate password is just attempting to unwrap that blob:

```
Duress::check(candidate)
    → read blob from NVS
    → IdentityCrypto::unwrap(candidate, blob) → true iff candidate was the
      password used in setup() (the HMAC only verifies for that one key)
```

The duress password never derives any real key and has no relationship to
the identity, message, contact, or settings encryption — its only job is
"does this string match what was configured," which is also why a wrong
guess can't accidentally decrypt anything.

The verifier is stored in NVS only (not mirrored to flash/SD like the real
identity). Losing it just disables the feature — recoverable by setting a
new one — unlike losing the real identity, which is catastrophic and is
why that one *is* mirrored three ways.

## Where it's checked at boot

Inside `runPasswordGate()`'s unlock loop, after each submitted password:

```
1. Try IdentityCrypto::unwrap(submitted, realIdentityBlob)
     → success: normal unlock, proceed to boot.
2. If that failed AND Duress::isConfigured():
     Try Duress::check(submitted)
     → success: secure-zero the input, FactoryWipe::wipeAll(), ESP.restart().
                 Never returns.
3. Otherwise: normal wrong-password handling (attempt counter, lockout).
```

The real password is always tried first, so on the (should-never-happen)
chance the two were ever made to collide, unlocking wins over wiping. The
duress check only runs at all if the real one already failed, and only
runs if a duress password is actually configured — zero added latency for
users who haven't set one up.

One side effect worth knowing: while a duress password *is* configured,
every wrong-password attempt now pays for two PBKDF2 derivations instead
of one (the real check, then the duress check) — roughly 4-6 seconds
instead of 2-3 before the next attempt is accepted. This doesn't affect a
correct real-password unlock, only failed attempts.

## Wipe scope

`FactoryWipe::wipeAll()` is the same routine used by the user-initiated
Settings → Factory Reset, so the two can't drift out of sync:

1. Clears every NVS namespace the firmware writes to (`ratcom`,
   `ratcom_cfg`, `ratcom_id`, `ratcom_duress`).
2. Wipes the SD card's `/ratcom` data tree.
3. Formats the flash LittleFS partition.

See [storage-layer.md](storage-layer.md) for what lives in each tier.

## Settings UI

Settings → Security shows the current state and lets you set, reset, or
disable the duress password:

- **Disabled** → selecting the row opens a small popup (drawn on top of the
  Security menu, not a full-screen takeover) asking for the new password,
  then a confirmation of it. Esc cancels at any point and discards
  whatever was typed so far — nothing is written unless both fields are
  entered, match, meet the minimum length, and differ from the real
  at-rest password (checked via `ReticulumManager::isCurrentPassword()`,
  comparing against the password cached in RAM for this session).
- **Enabled** → the menu also shows "Reset Duress Password" (same popup
  flow, just overwrites the existing verifier — no need to disable first)
  and selecting the main row asks for a Y/N confirmation before disabling.

## Limitations

- **Weaker, not absent, against an adversary who has reverse-engineered
  this firmware.** The entire mechanism is visible in this source tree, so
  someone who has read it and checks whether the data is still present
  after an unlock attempt can be reasonably confident a wipe happened. But
  confidence isn't proof: the boot screen is identical whether the device
  is factory-fresh, was factory-reset deliberately, or was duress-wiped,
  and nothing logs or timestamps which one occurred. The user can still
  deny that *this specific unlock attempt* caused it — that's a weaker,
  circumstantial deniability than against someone who hasn't read the
  source, not zero deniability. See [threat-model.md](threat-model.md).
- **Irreversible.** There is no undo — by design, the same as Factory
  Reset.
- **Verifier is NVS-only**, not mirrored across tiers (see above) — an
  acceptable trade-off since the failure mode is "feature silently
  disabled," not data loss.
