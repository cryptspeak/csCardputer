#pragma once

#include <M5GFX.h>
#include <Arduino.h>

class FlashStore;
class SDStore;

// =============================================================================
// rsCardputer — compact field-console theme
//
// Colors below are runtime-mutable (not constexpr) so a user-selected theme
// can override them at boot, but every symbol keeps its original name and
// type — every existing call site (Theme::PRIMARY, etc.) keeps working
// unchanged. Defaults match the original hand-tuned palette exactly; custom
// themes only take effect after Theme::load() runs and replaces these.
// =============================================================================

namespace Theme {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Linear-interpolates two RGB565 colors channel-by-channel (t=0 -> a,
// t=1 -> b). Used for fade-in/out effects (e.g. the boot screen reveal)
// where M5GFX has no per-pixel alpha blending to lean on instead.
inline uint16_t lerp565(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t);
    int g = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// --- Colors (RGB565) — mutable, see Theme.cpp for defaults ---
extern uint16_t BG;
extern uint16_t BG_ELEVATED;
extern uint16_t BG_SURFACE;
extern uint16_t BG_HOVER;
extern uint16_t TEXT_PRIMARY;
extern uint16_t TEXT_SECONDARY;
extern uint16_t TEXT_MUTED;
extern uint16_t PRIMARY;
extern uint16_t PRIMARY_MUTED;
extern uint16_t PRIMARY_SUBTLE;
extern uint16_t SECONDARY;
extern uint16_t MUTED;
extern uint16_t SUCCESS;
extern uint16_t ERROR;
extern uint16_t WARNING;
extern uint16_t ACCENT;
extern uint16_t BORDER;
extern uint16_t DIVIDER;
extern uint16_t SELECTION_BG;
extern uint16_t BAR_BG;
extern uint16_t TAB_ACTIVE;
extern uint16_t TAB_INACTIVE;
extern uint16_t BADGE_BG;
extern uint16_t BADGE_TEXT;

// --- Custom theme support ---
// The 7 base hues a user can edit; every other color above is derived from
// these (see Theme.cpp). Stored/edited as 6-digit hex strings ("RRGGBB").
struct BaseColors {
    String name = "Default";
    String bg, text, primary, secondary, accent, error, warning;
};

struct Preset {
    const char* name;
    const char* bg, *text, *primary, *secondary, *accent, *error, *warning;
};

extern const Preset PRESETS[];
extern const int PRESET_COUNT;

// Current base colors (for the settings UI to display/edit).
BaseColors current();

// Validates each hex field (must be exactly 6 hex digits; invalid fields
// fall back to the current value for that field only) and applies +
// derives the full palette. Does not save to disk.
void apply(const BaseColors& colors);

// Restores the compiled-in default palette exactly (bypasses derivation).
void resetToDefaults();

// Index into PRESETS, or -1 if the active colors don't match any preset
// (i.e. a custom theme is active).
int activePresetIndex();

// Loads theme.json from SD (if ready) else flash, validates, and applies.
// Safe to call with sd == nullptr (flash-only, used early at boot before
// the SD card is mounted) and safe to call again later once SD is ready —
// a second call re-applies whichever tier currently has a saved theme.
// Never reads identity/encryption state: the theme file is intentionally
// plaintext, since it carries no sensitive information and the boot/lock
// screens need it before the user unlocks anything.
// Returns true if a saved theme was found and applied (false = defaults).
bool load(FlashStore* flash, SDStore* sd);

// Persists the current colors as plaintext JSON to flash, and to SD too
// if it's ready.
bool save(FlashStore* flash, SDStore* sd);

// --- Layout Metrics ---
constexpr int SCREEN_W       = 240;
constexpr int SCREEN_H       = 135;
constexpr int STATUS_BAR_H   = 14;
constexpr int TAB_BAR_H      = 18;
constexpr int CONTENT_Y      = STATUS_BAR_H;
constexpr int CONTENT_H      = SCREEN_H - STATUS_BAR_H - TAB_BAR_H;
constexpr int CONTENT_W      = SCREEN_W;

// --- Font ---
constexpr const lgfx::GFXfont* FONT_SMALL = nullptr;  // Built-in 6x8
constexpr int FONT_SIZE       = 1;
constexpr int CHAR_W          = 6;
constexpr int CHAR_H          = 8;
constexpr int UI_FONT_H       = 10;
constexpr int LIST_ROW_H      = 14;
constexpr int SECTION_HEADER_H = 15;
constexpr int SHELL_TEXT_Y    = 3;

// --- Tab Bar ---
constexpr int TAB_COUNT       = 4;
constexpr int TAB_W           = SCREEN_W / TAB_COUNT;

// --- Status Bar ---
constexpr int STATUS_PAD      = 2;

inline void useSmallFont(M5Canvas& canvas) {
    canvas.setFont(nullptr);
    canvas.setTextSize(1);
}

inline void useUiFont(M5Canvas& canvas) {
    canvas.setFont(nullptr);
    canvas.setTextSize(1.2f);
}

}  // namespace Theme
