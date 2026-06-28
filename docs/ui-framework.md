# UI Framework

Modules: [`src/ui/Screen.h`](../src/ui/Screen.h),
[`src/ui/UIManager.{h,cpp}`](../src/ui/UIManager.h),
[`src/ui/StatusBar.{h,cpp}`](../src/ui/StatusBar.h),
[`src/ui/TabBar.{h,cpp}`](../src/ui/TabBar.h),
[`src/ui/widgets/`](../src/ui/widgets/)

This covers the screen/widget framework structure. Color theming
specifically (presets, the custom RGB mixer, the RGB332 canvas
truncation quirk) is already documented in depth in
[theme-config.md](theme-config.md) — not repeated here.

## The `Screen` contract

```cpp
class Screen {
    virtual void render(M5Canvas&) = 0;            // required
    virtual const char* title() const = 0;          // required
    virtual void onEnter() {}                        // optional
    virtual void onExit() {}                          // optional
    virtual bool handleKey(const KeyEvent&) { return false; }  // optional
    virtual void tick() {}                             // optional
};
```

Every screen under `src/ui/screens/` implements `render()` and
`title()`; the rest default to no-ops. `tick()` is the odd one out —
it runs on **every** main-loop iteration regardless of render timing
or dirty state, specifically for logic that needs continuous polling
rather than discrete key events. It only has one real consumer today:
`SettingsScreen`'s accelerating hold-to-repeat color mixer (see
[theme-config.md](theme-config.md#accelerating-hold-to-repeat-mix-mode)).
Every other screen pays its cost (one virtual call per loop iteration)
for free since it's a no-op by default.

## `UIManager`: canvas, dirty flags, render/flush

A single `M5Canvas` sprite (240×135, 8 bits per pixel) is the only
thing actually drawn to the physical display. `setColorDepth(8)` does
**not** allocate an indexed palette here — at exactly 8bpp, M5GFX
renders RGB332 truecolor (3/3/2 bits per channel) instead, so
`UIManager::refreshPalette()`'s `setPaletteColor()` calls are silent
no-ops. This is the same detail [theme-config.md](theme-config.md#why-the-step-size-ramps-up-at-all)
explains in depth from the angle of "why a custom color slider has to
move by more than 1 unit per hold-tick to be visible" — here it just
matters that **every color change requires a full redraw**
(`markAllDirty()`) to become visible, since there's no palette
indirection a single `setPaletteColor()` call could exploit instead.

Four independent dirty flags gate what gets redrawn:

| Flag | Region |
|---|---|
| `markStatusDirty()` | Status bar (top strip) |
| `markTabDirty()` | Tab bar (bottom strip) |
| `markContentDirty()` | Active screen's content area (everything between) |
| `markAllDirty()` | All three, plus forces a full repaint |

`render()` is a fast no-op if no flag is set; otherwise it redraws
exactly the regions whose flags are set, then `flush()` pushes the
whole sprite to the physical display in one `pushSprite()` call. Boot
mode (`setBootMode(true)`) bypasses dirty-checking entirely and always
does a full repaint — appropriate during the boot sequence, where
screens change rapidly and partial-redraw bookkeeping isn't worth it.

`setScreen(Screen*)` calls the outgoing screen's `onExit()`, the
incoming screen's `onEnter()`, then unconditionally `markAllDirty()` —
a screen switch is always a full repaint, never a partial one.

### Overlay

`setOverlay(Screen*)` registers a second screen (used today only by
`HelpOverlay`) that renders **after**, and on top of, the active
screen's content — clipped to the content area, so it can never paint
over the status or tab bars. `main.cpp` treats overlay visibility as
higher priority than the active screen for key routing — see below.

## Input routing

Key events reach the UI through a fixed precedence chain, assembled in
`main.cpp::loop()`, not inside `UIManager` itself:

```
1. Boot mode?           → goes straight to ui.handleKey(), nothing else runs
2. Help overlay visible? → overlay gets the key first
3. HotkeyManager.process() → Ctrl+<letter> shortcuts, consumes if matched
4. ui.handleKey()          → routes to the active Screen::handleKey()
5. Not consumed, not Ctrl, character is ',' or '/' → tab-cycle fallback
```

`Screen::handleKey()`'s return value is the propagation signal: `true`
means "I used this key, stop here"; `false` lets it fall through to the
next stage. `UIManager::handleKey()` always calls `markContentDirty()`
before dispatching, on the assumption that almost every keypress
changes something worth redrawing — cheaper to over-mark-dirty than to
have every screen remember to do it themselves.

## Widgets

[`src/ui/widgets/`](../src/ui/widgets/) — three small, reusable pieces
shared across screens rather than each screen reimplementing its own
list/input handling:

- **`ProgressBar`** — stateless, read-only; `render()` takes an
  explicit progress value and an optional fade factor (used for the
  boot screen's reveal animation). No input handling at all.
- **`ScrollList`** — the backing widget for every menu/list screen
  (Messages, Nodes, Settings, Radio Setup). Owns selection index and
  scroll offset together so the selected row is always kept in view;
  wrapping at both ends (`scrollDown()` past the last item wraps to
  index 0, `scrollUp()` past the first wraps to the last). Per-item
  color override exists specifically for things like Settings rows
  that need to show one entry in a warning color without a whole
  separate rendering path.
- **`TextInput`** — single-line text entry with cursor, optional
  `numericOnly` mode (radio frequency, SF, etc. fields all use this
  rather than free text), a configurable max length, and
  left-scrolling once typed text exceeds the visible width.

## Screens

Each file under [`src/ui/screens/`](../src/ui/screens/) is one
`Screen` implementation:

| Screen | Purpose |
|---|---|
| `BootScreen` | Boot progress bar + the intro wipe/decrypt animation (see [boot-sequence.md](boot-sequence.md)) |
| `PasswordScreen` | Setup/unlock password entry, lockout display (see [encryption-identity.md](encryption-identity.md)) |
| `MigrationWarningScreen` | Legacy-identity migration choice + progress (see [boot-sequence.md](boot-sequence.md)) |
| `RadioSetupScreen` | First-boot LoRa parameter wizard (region preset + frequency/SF/BW/CR/power) — system time comes from GPS instead of a timezone choice |
| `NameInputScreen` | First-boot/Settings display name entry |
| `DataCleanScreen` | First-boot prompt to wipe leftover data from a previous install |
| `HomeScreen` | Main dashboard — identity, transport/radio status, manual announce |
| `MessagesScreen` | Conversation list, unread badges, add/remove contact |
| `MessageView` | Single conversation thread + composer |
| `NodesScreen` | Discovered/saved peers (backed by `AnnounceManager`, see [announce-discovery.md](announce-discovery.md)) |
| `SettingsScreen` | Nested settings menus — Radio, WiFi, TCP, SD, Display, Audio, Time, Security, Theme, About (see [duress-password.md](duress-password.md) for the Security submenu) |
| `HelpOverlay` | Hotkey reference, rendered as an overlay over whatever screen is active |
