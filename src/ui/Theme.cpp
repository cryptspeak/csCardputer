#include "Theme.h"
#include <ArduinoJson.h>
#include <cctype>
#include <cstring>
#include "storage/FlashStore.h"
#include "storage/SDStore.h"
#include "config/Config.h"

namespace Theme {

// --- Compiled-in defaults (also the "Default" preset's base hues) ---
namespace {
constexpr const char* DEFAULT_BG        = "05080A";
constexpr const char* DEFAULT_TEXT      = "E4F3F0";
constexpr const char* DEFAULT_PRIMARY   = "00E06D";
constexpr const char* DEFAULT_SECONDARY = "91A8A5";
constexpr const char* DEFAULT_ACCENT    = "4FD7FF";
constexpr const char* DEFAULT_ERROR     = "FF5C6C";
constexpr const char* DEFAULT_WARNING   = "F0C04F";

// Max plausible theme.json size. Real payload is ~150 bytes; this just
// bounds parse cost/heap use against a corrupted or hostile file — it is
// not a meaningful security boundary, just cheap defense-in-depth.
constexpr size_t MAX_THEME_FILE_BYTES = 2048;
}  // namespace

uint16_t BG, BG_ELEVATED, BG_SURFACE, BG_HOVER;
uint16_t TEXT_PRIMARY, TEXT_SECONDARY, TEXT_MUTED;
uint16_t PRIMARY, PRIMARY_MUTED, PRIMARY_SUBTLE;
uint16_t SECONDARY, MUTED, SUCCESS;
uint16_t ERROR, WARNING, ACCENT;
uint16_t BORDER, DIVIDER, SELECTION_BG, BAR_BG;
uint16_t TAB_ACTIVE, TAB_INACTIVE, BADGE_BG, BADGE_TEXT;

const Preset PRESETS[] = {
    {"Default",       DEFAULT_BG, DEFAULT_TEXT, DEFAULT_PRIMARY, DEFAULT_SECONDARY, DEFAULT_ACCENT, DEFAULT_ERROR, DEFAULT_WARNING},
    {"Amber Retro",   "0A0704", "F3E4C8", "F0A030", "A89070", "FFD060", "FF5C6C", "F0C04F"},
    {"Ocean Blue",    "030B14", "DCEEF8", "2090FF", "7098B0", "60E0FF", "FF6868", "F0C050"},
    {"Mono",          "0A0A0A", "E8E8E8", "C0C0C0", "909090", "FFFFFF", "FF7878", "D8D8D8"},
    {"High Contrast", "000000", "FFFFFF", "00FF66", "C0C0C0", "00DDFF", "FF3344", "FFCC00"},
    // Purple + green high-contrast
    {"Voidframe",      "0A0612", "EDE6FF", "39FF8C", "9080A8", "B341FF", "FF4466", "FFC247"},
    // Full purple, layered shades
    {"Nightshade",     "0C0716", "E6DCFF", "9D5CFF", "85749E", "D86CFF", "FF5C8A", "F0B95C"},
    // Classic monochrome CRT phosphor
    {"Phosphor",       "020503", "C8FFD8", "33FF66", "5C8C68", "7CFFD0", "FF5544", "FFB347"},
    // Cyan/magenta duotone
    {"Glitchline",     "05060E", "E0F7FF", "33E5FF", "7A8AB0", "FF2EC4", "FF4466", "FFCB47"},
};
const int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

static BaseColors g_current;  // last-applied base colors (for the UI + save())

// --- Small color-math helpers (cosmetic only — no precision requirements) ---
namespace {

uint8_t clampByte(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

bool hexNibble(char c, uint8_t& out) {
    if (c >= '0' && c <= '9') { out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { out = 10 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'F') { out = 10 + (c - 'A'); return true; }
    return false;
}

// Strict: exactly 6 hex digits, nothing else. Returns false (leaves rgb
// untouched) on any malformed input — callers must supply a safe fallback.
bool parseHex6(const String& s, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (s.length() != 6) return false;
    uint8_t n[6];
    for (int i = 0; i < 6; i++) {
        if (!hexNibble(s[i], n[i])) return false;
    }
    r = (uint8_t)((n[0] << 4) | n[1]);
    g = (uint8_t)((n[2] << 4) | n[3]);
    b = (uint8_t)((n[4] << 4) | n[5]);
    return true;
}

struct RGB { uint8_t r, g, b; };

RGB rgbOf(const String& hex, const char* fallback) {
    uint8_t r, g, b;
    if (parseHex6(hex, r, g, b)) return {r, g, b};
    parseHex6(fallback, r, g, b);
    return {r, g, b};
}

uint16_t lighten(RGB c, int amount) {
    return rgb565(clampByte(c.r + amount), clampByte(c.g + amount), clampByte(c.b + amount));
}

uint16_t blend(RGB a, RGB b, float t) {
    return rgb565(clampByte((int)(a.r + (b.r - a.r) * t)),
                  clampByte((int)(a.g + (b.g - a.g) * t)),
                  clampByte((int)(a.b + (b.b - a.b) * t)));
}

uint16_t scale(RGB c, float t) {
    return rgb565(clampByte((int)(c.r * t)), clampByte((int)(c.g * t)), clampByte((int)(c.b * t)));
}

uint16_t pack(RGB c) { return rgb565(c.r, c.g, c.b); }

}  // namespace

void resetToDefaults() {
    BaseColors d;
    d.name = "Default";
    d.bg = DEFAULT_BG; d.text = DEFAULT_TEXT; d.primary = DEFAULT_PRIMARY;
    d.secondary = DEFAULT_SECONDARY; d.accent = DEFAULT_ACCENT;
    d.error = DEFAULT_ERROR; d.warning = DEFAULT_WARNING;
    g_current = d;

    // Exact original hand-tuned constants — bypasses derivation so the
    // out-of-the-box look never regresses even slightly.
    BG             = rgb565(5, 8, 10);
    BG_ELEVATED    = rgb565(12, 20, 24);
    BG_SURFACE     = rgb565(18, 29, 35);
    BG_HOVER       = rgb565(24, 48, 51);
    TEXT_PRIMARY   = rgb565(228, 243, 240);
    TEXT_SECONDARY = rgb565(145, 168, 165);
    TEXT_MUTED     = rgb565(82, 104, 102);
    PRIMARY        = rgb565(0, 224, 109);
    PRIMARY_MUTED  = rgb565(0, 168, 83);
    PRIMARY_SUBTLE = rgb565(6, 37, 26);
    SECONDARY      = TEXT_SECONDARY;
    MUTED          = TEXT_MUTED;
    SUCCESS        = rgb565(49, 233, 129);
    ERROR          = rgb565(255, 92, 108);
    WARNING        = rgb565(240, 192, 79);
    ACCENT         = rgb565(79, 215, 255);
    BORDER         = rgb565(33, 56, 59);
    DIVIDER        = rgb565(16, 35, 41);
    SELECTION_BG   = PRIMARY_SUBTLE;
    BAR_BG         = rgb565(7, 16, 20);
    TAB_ACTIVE     = TEXT_PRIMARY;
    TAB_INACTIVE   = TEXT_SECONDARY;
    BADGE_BG       = ERROR;
    BADGE_TEXT     = BG;
}

void apply(const BaseColors& colors) {
    // Each field falls back independently to the *current* value of that
    // same field if malformed — a partially-corrupt file only loses the
    // fields that are actually bad, not the whole theme.
    RGB bg   = rgbOf(colors.bg,        g_current.bg.length()        ? g_current.bg.c_str()        : DEFAULT_BG);
    RGB text = rgbOf(colors.text,      g_current.text.length()      ? g_current.text.c_str()      : DEFAULT_TEXT);
    RGB prim = rgbOf(colors.primary,   g_current.primary.length()   ? g_current.primary.c_str()   : DEFAULT_PRIMARY);
    RGB sec  = rgbOf(colors.secondary, g_current.secondary.length() ? g_current.secondary.c_str() : DEFAULT_SECONDARY);
    RGB acc  = rgbOf(colors.accent,    g_current.accent.length()    ? g_current.accent.c_str()    : DEFAULT_ACCENT);
    RGB err  = rgbOf(colors.error,     g_current.error.length()     ? g_current.error.c_str()     : DEFAULT_ERROR);
    RGB warn = rgbOf(colors.warning,   g_current.warning.length()   ? g_current.warning.c_str()   : DEFAULT_WARNING);

    char buf[7];
    auto toHex = [&](RGB c) -> String {
        snprintf(buf, sizeof(buf), "%02X%02X%02X", c.r, c.g, c.b);
        return String(buf);
    };
    g_current.name = colors.name.length() ? colors.name : String("Custom");
    g_current.bg = toHex(bg); g_current.text = toHex(text); g_current.primary = toHex(prim);
    g_current.secondary = toHex(sec); g_current.accent = toHex(acc);
    g_current.error = toHex(err); g_current.warning = toHex(warn);

    // If this exactly matches the compiled default, use the exact literal
    // palette (zero drift). Otherwise derive supporting shades formulaically
    // — close in spirit to the hand-tuned defaults, good enough for a
    // cosmetic feature, and always internally consistent for any hue picked.
    if (g_current.bg == DEFAULT_BG && g_current.text == DEFAULT_TEXT &&
        g_current.primary == DEFAULT_PRIMARY && g_current.secondary == DEFAULT_SECONDARY &&
        g_current.accent == DEFAULT_ACCENT && g_current.error == DEFAULT_ERROR &&
        g_current.warning == DEFAULT_WARNING) {
        resetToDefaults();
        g_current.name = colors.name.length() ? colors.name : String("Default");
        return;
    }

    BG             = pack(bg);
    BG_ELEVATED    = blend(bg, text, 0.06f);
    BG_SURFACE     = blend(bg, text, 0.11f);
    BG_HOVER       = blend(bg, text, 0.17f);
    TEXT_PRIMARY   = pack(text);
    TEXT_SECONDARY = pack(sec);
    TEXT_MUTED     = blend(text, bg, 0.65f);
    PRIMARY        = pack(prim);
    PRIMARY_MUTED  = scale(prim, 0.75f);
    PRIMARY_SUBTLE = blend(prim, bg, 0.85f);
    SECONDARY      = TEXT_SECONDARY;
    MUTED          = TEXT_MUTED;
    SUCCESS        = lighten(prim, 18);
    ERROR          = pack(err);
    WARNING        = pack(warn);
    ACCENT         = pack(acc);
    BORDER         = blend(bg, text, 0.20f);
    DIVIDER        = blend(bg, text, 0.10f);
    SELECTION_BG   = PRIMARY_SUBTLE;
    BAR_BG         = blend(bg, text, 0.03f);
    TAB_ACTIVE     = TEXT_PRIMARY;
    TAB_INACTIVE   = TEXT_SECONDARY;
    BADGE_BG       = ERROR;
    BADGE_TEXT     = BG;
}

BaseColors current() {
    if (g_current.bg.isEmpty()) resetToDefaults();
    return g_current;
}

int activePresetIndex() {
    BaseColors c = current();
    for (int i = 0; i < PRESET_COUNT; i++) {
        const Preset& p = PRESETS[i];
        if (c.bg.equalsIgnoreCase(p.bg) && c.text.equalsIgnoreCase(p.text) &&
            c.primary.equalsIgnoreCase(p.primary) && c.secondary.equalsIgnoreCase(p.secondary) &&
            c.accent.equalsIgnoreCase(p.accent) && c.error.equalsIgnoreCase(p.error) &&
            c.warning.equalsIgnoreCase(p.warning)) {
            return i;
        }
    }
    return -1;
}

// --- Plaintext JSON persistence -------------------------------------------
// Intentionally NOT routed through AtRestCrypto/SettingsEncryption: color
// values carry no sensitive information (see docs/theme-config.md), and the
// boot/password-gate screens need a theme before any identity is unlocked.
// The only data taken from this file is six-hex-digit color strings, each
// independently validated and clamped to 0-255 per channel before use —
// there is no path from file content to memory size, indexing, or control
// flow, so a corrupted or maliciously edited file can only produce an
// ugly-but-safe color combination, never a crash or code-execution surface.

namespace {

bool parseThemeJson(const String& json, BaseColors& out) {
    if (json.length() == 0 || json.length() > MAX_THEME_FILE_BYTES) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;

    BaseColors base = current();  // fields default to current value if missing/bad
    const char* name = doc["name"] | "Custom";
    base.name = String(name).substring(0, 24);

    auto field = [&](const char* key, String& target) {
        const char* v = doc[key] | "";
        if (v && strlen(v) == 6) {
            bool ok = true;
            for (int i = 0; i < 6 && ok; i++) ok = isxdigit((unsigned char)v[i]);
            if (ok) target = String(v);
        }
    };
    field("bg", base.bg);
    field("text", base.text);
    field("primary", base.primary);
    field("secondary", base.secondary);
    field("accent", base.accent);
    field("error", base.error);
    field("warning", base.warning);

    out = base;
    return true;
}

String serializeThemeJson(const BaseColors& c) {
    JsonDocument doc;
    doc["name"] = c.name;
    doc["bg"] = c.bg;
    doc["text"] = c.text;
    doc["primary"] = c.primary;
    doc["secondary"] = c.secondary;
    doc["accent"] = c.accent;
    doc["error"] = c.error;
    doc["warning"] = c.warning;
    String out;
    serializeJson(doc, out);
    return out;
}

}  // namespace

bool load(FlashStore* flash, SDStore* sd) {
    String raw;
    bool fromSD = false;

    if (sd && sd->isReady()) {
        raw = sd->readString(SD_PATH_THEME_CONFIG);
        if (raw.length() > 0) fromSD = true;
    }
    if (raw.length() == 0 && flash) {
        raw = flash->readString(PATH_THEME_CONFIG);
    }
    if (raw.length() == 0) {
        resetToDefaults();
        return false;
    }

    BaseColors parsed;
    if (!parseThemeJson(raw, parsed)) {
        Serial.println("[THEME] Saved theme.json invalid/corrupt — using defaults");
        resetToDefaults();
        return false;
    }

    apply(parsed);
    Serial.printf("[THEME] Loaded '%s' from %s\n", g_current.name.c_str(), fromSD ? "SD" : "flash");

    // Backfill flash from an SD-only theme. The very first Theme::load() of
    // boot runs flash-only, before SD is mounted (see main.cpp) — if the
    // saved theme only exists on SD (e.g. it was set before this backfill
    // existed, or a Settings save happened while flash was briefly
    // unavailable), that early call finds nothing and the boot screen
    // renders the compiled-in default palette for the ~1s it takes to get
    // through the intro animation and radio init before SD mounts and this
    // function runs again. Comparing against flash's current copy first
    // keeps this a no-op on every normal boot once the two tiers agree.
    if (fromSD && flash) {
        String flashRaw = flash->readString(PATH_THEME_CONFIG);
        if (flashRaw != raw) {
            flash->writeString(PATH_THEME_CONFIG, raw);
        }
    }

    return true;
}

bool save(FlashStore* flash, SDStore* sd) {
    String json = serializeThemeJson(current());
    bool ok = false;

    if (sd && sd->isReady()) {
        sd->ensureDir("/ratcom");
        sd->ensureDir("/ratcom/config");
        if (sd->writeString(SD_PATH_THEME_CONFIG, json)) ok = true;
    }
    if (flash && flash->writeString(PATH_THEME_CONFIG, json)) ok = true;

    return ok;
}

}  // namespace Theme
