# Network Interfaces & Radio

Modules: [`src/transport/`](../src/transport/), [`src/radio/SX1262.{h,cpp}`](../src/radio/SX1262.h),
[`src/hal/SharedSPIBus.{h,cpp}`](../src/hal/SharedSPIBus.h)

Five `RNS::Interface` implementations exist; the firmware can have
several active simultaneously (LoRa is always on if the radio is
present; WiFi AP/STA, up to `MAX_TCP_CONNECTIONS` TCP clients, and
AutoInterface are all independently optional, driven by `UserConfig`).
This doc covers each one's framing/lifecycle, the SX1262 driver
underneath LoRa, and the SPI arbitration all of them share with the SD
card.

## SharedSPIBus — why it exists

The Cardputer Adv puts the SX1262 radio and the SD card on the same
physical SPI bus (FSPI/SPI2 — see `BoardConfig.h`), with the display on
a separate bus (HSPI/SPI3) specifically so display refreshes can't
glitch radio SPI transactions (see the comment at the top of
`main.cpp`). Radio and SD *do* share a bus, though, and both can be
touched from different call sites in the same loop iteration (e.g. the
radio ISR-driven receive path and a contacts/message file read).
`SharedSPILock` is a RAII wrapper around one FreeRTOS **recursive**
mutex — recursive so a call chain that re-enters a locked section on
the same task doesn't deadlock itself. Every SX1262 register operation
and every SD read/write that matters is wrapped in one:

```cpp
SharedSPILock lock;
if (!lock.locked()) return;  // timed out or mutex unavailable
// ... SPI transaction ...
```

The default timeout is `portMAX_DELAY` (wait forever) — there's no
attempt at a soft-fail path if the bus is contended; the design intent
is that SPI transactions are short enough this never matters in
practice.

## LoRaInterface

[`src/transport/LoRaInterface.{h,cpp}`](../src/transport/LoRaInterface.h)

Bidirectional `RNS::InterfaceImpl`, `_HW_MTU = RNS::Type::Reticulum::MTU`
(500 bytes). The SX1262's own max packet size is 255 bytes, so anything
over `RNODE_SINGLE_MTU` (254 bytes, leaving 1 byte for the on-air
header) is split into two LoRa frames and reassembled — see below.

### On-air framing

Every LoRa frame carries a single header byte, borrowed from the RNode
firmware framing convention: upper nibble is a random per-packet
sequence number, lower nibble carries flags (currently only
`RNODE_FLAG_SPLIT`). The sequence number's only job is letting the
receiver match a split packet's second frame to its first — it is not
a general retransmission/ack sequence number.

```
TX, payload ≤ 254B:  [header][payload]                        — one frame
TX, payload > 254B:  [header|SPLIT][first 254B]                — frame 1
                      [header|SPLIT][remaining bytes]           — frame 2 (same seq nibble)
```

Frame 2 is sent from `loop()` immediately after frame 1's TX completes
(`isTxBusy()` goes false) — not queued behind other outgoing traffic,
so a split packet's two halves go out back-to-back.

On the receive side, a frame tagged `SPLIT` starts a pending
reassembly buffer if none is in progress; a second frame with the
**same** sequence nibble appends to it and delivers the reassembled
payload to `InterfaceImpl::handle_incoming()`. If a second `SPLIT`
frame arrives with a **different** sequence nibble while a reassembly
is still pending, the in-progress buffer is discarded — logged
explicitly ("previous frame 2 lost") — and reassembly restarts from the
new frame. A 5-second timeout (`SPLIT_RX_TIMEOUT_MS`) also discards a
stale pending reassembly if its second frame never arrives at all. Both
paths are a deliberate "drop and move on" choice, not a bug: LXMF
already has its own retry logic above this layer (see
[lxmf-messaging.md](lxmf-messaging.md)), so silently failing one
oversized packet and letting the sender's retry pick it up again is
cheaper than building reliability into the link layer too. Two
**different senders'** split packets colliding mid-reassembly (same
random sequence nibble, interleaved frames) is the one case this can't
distinguish from a single sender's lost frame — rare given the nibble's
16 values and LoRa's slow airtime, but worth knowing if reassembly
failures ever show up in the field.

### TX queueing and airtime throttling

A 4-deep `std::deque` (`TX_QUEUE_MAX`) buffers outgoing packets while a
transmission, split-TX second frame, or a pending split-RX reassembly
is in progress, rather than dropping them outright. If the queue is
already full, the **oldest** queued packet is dropped to make room for
the new one (logged) — recency is prioritized over delivery order under
sustained pressure.

`airtimeUtilization()` tracks a decaying-window estimate of how much of
the last 60 seconds (`AIRTIME_WINDOW_MS`) this device has spent
transmitting, via `SX1262::getAirtime()`'s per-packet estimate
accumulated in `_airtimeAccumMs`. `LoRaInterface::AIRTIME_THROTTLE`
(0.25 — a 25% duty cycle ceiling) is read by `main.cpp`'s periodic
announce logic to skip an announce outright if recent airtime usage is
already above that, rather than adding to congestion the device caused
itself.

## WiFiInterface

[`src/transport/WiFiInterface.{h,cpp}`](../src/transport/WiFiInterface.h)

Runs the device as a WiFi AP (`MAX_AP_CLIENTS` = 4) that Reticulum
clients can connect to over TCP, using an HDLC-like byte-stuffing frame
format: `0x7E` start/end delimiter, `0x7D` escape byte, escaped bytes
XORed with `0x20`. `_HW_MTU` is also 500 bytes; the TX buffer is sized
`600*2+2` to cover the worst case where every payload byte needs
escaping.

Each connected client gets its **own** `FrameState` (a small
`{inFrame, escaped, pos}` struct) in a parallel vector to `_clients`,
so frame reassembly persists correctly across `loop()` calls per
client and one client's partial frame can never be corrupted by
another's traffic. This is the same HDLC framing scheme
`TCPClientInterface` uses, but with per-client buffer state rather
than the shared buffers `TCPClientInterface` uses (see below).

## TCPClientInterface

[`src/transport/TCPClientInterface.{h,cpp}`](../src/transport/TCPClientInterface.h)

The client-side counterpart: connects outbound to a configured LXMF
hub (`UserConfig`'s `tcpConnections`, up to `MAX_TCP_CONNECTIONS` = 4
endpoints, each independently toggleable via `autoConnect`).

- **Async connect.** `tryConnect()` spawns a short-lived FreeRTOS task
  so a slow/hanging DNS lookup or TCP handshake can't block the main
  loop; `_connectState` (`CS_IDLE → CS_CONNECTING → CS_CONNECTED` or
  `CS_FAILED`) is the synchronization point the main loop polls.
- **Exponential backoff** on reconnect: starts at 1s, doubles up to a
  300s ceiling, with jitter — avoids every configured hub being
  hammered in lockstep if the device's WiFi link drops and reconnects.
- **Header2 transport-ID wrapping.** Reticulum's TCP transport uses an
  extra "Header2" framing layer (a 16-byte hub transport ID prepended
  to data packets) that this client learns passively from the hub's
  first incoming Header2 packet rather than needing it configured, and
  re-learns if a later incoming Header2 packet carries a different id
  (e.g. the hub regenerates its identity without dropping the TCP
  socket) so outgoing wraps never keep using a stale id. Announces and
  link packets bypass this wrapping; only ordinary data packets get it.
- **Keepalive-via-timeout.** `TCP_KEEPALIVE_TIMEOUT_MS` (120 seconds,
  chosen to survive typical mobile-NAT idle timeouts) is not a TCP
  keepalive option — it's an application-level "no bytes received in
  this long → force a reconnect" check in `loop()`.
- **Per-instance RX/TX buffers.** `_rxBuffer`, `_txBuffer`, and
  `_wrapBuffer` (`TCPClientInterface.h`) are allocated per instance in
  the constructor and freed in the destructor. They used to be `static`
  and shared across every `TCPClientInterface` instance to save ~8KB
  per additional configured hub, but that's unsafe with more than one
  hub connected at once (`main.cpp`'s `reloadTCPClients()` can run up to
  `MAX_TCP_CONNECTIONS` simultaneously): one connection's in-flight
  frame would silently corrupt another's. Per-instance buffers cost the
  ~8KB/hub the sharing saved, but correctness wins given this is a
  reachable multi-hub configuration, not a hypothetical.

## AutoInterfaceWrapper

[`src/transport/AutoInterfaceWrapper.{h,cpp}`](../src/transport/AutoInterfaceWrapper.h)

A thin wrapper gluing the firmware's WiFi STA lifecycle to
microReticulum's `RNS::AutoInterface` (multicast UDP discovery over
IPv6 link-local addresses — Reticulum's "find other Reticulum nodes on
this LAN automatically" mechanism). The wrapper exists because
`AutoInterface` needs a valid link-local IPv6 address before it can
start, and that address isn't available the instant WiFi associates —
SLAAC (stateless address autoconfiguration) takes anywhere from
~1.5 to ~10 seconds. `main.cpp` polls for a `fe80::/10` address after
STA connects and only calls `start()` once one appears, with a 10-second
giving-up timeout. `notifyLinkChange()` is also polled periodically
after that, since a privacy-extension IPv6 address can rotate while
the device stays associated to the same AP — `AutoInterface`'s own
`notify_link_change()` is idempotent, so polling it on every check is
cheap and only does real work on an actual address change.

## BLEStub

**Status: not implemented.** The source marks it for a future
"v1.1" — a version label inherited from the original upstream codebase
this firmware was forked from, not a committed date on this fork's own
release line (see `Config.h`'s independent versioning note). Treat
this as "not yet built," not as a scheduled feature.

[`src/transport/BLEStub.{h,cpp}`](../src/transport/BLEStub.h)

Exactly what the name says: a minimal BLE GATT service (one fixed
UUID, a write characteristic and a notify characteristic) that
advertises but implements no Reticulum interface and no protocol —
`loop()` is a no-op with a comment marking it for a future "Sideband
protocol" implementation. `main.cpp` explicitly logs `[BLE] Disabled
(stub — v1.1)` at boot. There is nothing to configure or debug here
yet; it's a placeholder, not a partially-working feature.

## SX1262 driver

[`src/radio/SX1262.{h,cpp}`](../src/radio/SX1262.h),
[`src/radio/RadioConstants.h`](../src/radio/RadioConstants.h)

A from-scratch SPI opcode/register driver for the Semtech SX1262 (not
a vendored library), carrying over several hardware-specific
workarounds from the SX1262 datasheet errata and from RNode firmware's
own field-tested tuning:

- **IQ polarity register fix-up** — after every `setPacketParams()`
  call, the SX1262's IQ polarity register (`0x0736`) has to be
  corrected by hand (bit 2 set for standard IQ, cleared for inverted) —
  this is a documented SX1262 errata item, not something the chip
  handles internally when you set IQ-invert through the normal API.
- **TX clamp / antenna-mismatch workaround** — `setTxPower()` writes
  `0x0F` into bits [4:1] of the TX clamp config register
  (`REG_TX_CLAMP_CONFIG_6X`), per datasheet §15.2, to improve
  resilience against TX into a mismatched antenna load.
- **TCXO bring-up** — the Cap LoRa-1262 module uses an external TCXO,
  enabled via `DIO3` with a 640ms stabilization timeout before the PLL
  is calibrated; too short a timeout leaves the chip running off its
  internal RC oscillator instead.
- **LNA boost register** — `REG_LNA_6X` is written `0x96` directly;
  this value (and the PA config constants in `setTxPower()`) are
  carried over from RNode CE firmware's tuning for this RF front-end,
  not derived from a formula.
- **Image calibration by band** — `begin()` looks up frequency-band-
  specific calibration register values (separate tables for 430-440,
  470-510, 779-787, 863-870, and 902-928 MHz) rather than one global
  calibration; a frequency outside all five bands is rejected.
- **DIO2-as-RF-switch** — optionally configures the SX1262 to drive
  its own TX/RX antenna switching via `DIO2` (`LORA_DIO2_AS_RF_SWITCH`
  in `BoardConfig.h`), separate from the Cap module's external antenna
  enable (`enableCapLoRaRfSwitch()` in `main.cpp`, driven over I2C
  through a PI4IOE5V6408 IO expander — see
  [hardware-platform.md](hardware-platform.md)).

`getAirtime()` implements the standard LoRa airtime formula (symbol
rate from bandwidth/spreading-factor, payload symbol count adjusted for
low-data-rate-optimization at SF≥7, plus preamble/sync/header
overhead) and returns a millisecond estimate per packet — this is the
number `LoRaInterface::airtimeUtilization()` accumulates for the duty-
cycle throttle described above.
