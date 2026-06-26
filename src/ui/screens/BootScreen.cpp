#include "BootScreen.h"
#include "ui/Theme.h"
#include "ui/assets/BootLogo.h"
#include "config/Config.h"

namespace {

constexpr const char* TITLE = "CRYPTSPEAK";
constexpr int TITLE_LEN = 10;  // strlen(TITLE), known at compile time

// Decrypt-scramble timing: character i starts cycling through noise glyphs
// at i*TITLE_STAGGER_MS, scrambles for TITLE_SCRAMBLE_MS, then locks in.
constexpr uint32_t LOGO_WIPE_MS = 340;
constexpr uint32_t TITLE_STAGGER_MS = 40;
constexpr uint32_t TITLE_SCRAMBLE_MS = 210;
constexpr uint32_t TITLE_DONE_MS = (TITLE_LEN - 1) * TITLE_STAGGER_MS + TITLE_SCRAMBLE_MS;

constexpr char NOISE_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#%&$@*";
constexpr int NOISE_LEN = sizeof(NOISE_CHARS) - 1;

// Once the title has settled, the subtitle/version/bar/status fade in
// together over this long rather than popping in at full brightness.
constexpr uint32_t REVEAL_FADE_MS = 350;

// Cheap deterministic noise — same (i, tick) always gives the same glyph,
// so repeated render() calls within one ~45ms window are stable instead of
// flickering every call.
char noiseGlyph(int i, uint32_t tick) {
    uint32_t h = (uint32_t)i * 2654435761u ^ tick * 0x9E3779B1u;
    h ^= h >> 15;
    return NOISE_CHARS[h % NOISE_LEN];
}

}  // namespace

void BootScreen::setProgress(float progress, const char* status) {
    _progress = progress;
    _progressBar.setProgress(progress);
    if (status) _statusText = status;
}

void BootScreen::render(M5Canvas& canvas) {
    if (_introStartMs == 0) _introStartMs = millis();
    uint32_t t = millis() - _introStartMs;

    canvas.fillScreen(Theme::BG);

    // Cryptspeak mark, wiped in top-to-bottom on first boot and tinted with
    // the live accent color — recolors itself automatically whenever the
    // user picks a different theme/preset, since Theme::ACCENT is resolved
    // fresh on every render, not baked in.
    int logoX = (Theme::SCREEN_W - BootLogo::MARK_W) / 2;
    int logoY = 10;
    int revealH = (t >= LOGO_WIPE_MS)
        ? BootLogo::MARK_H
        : (int)((uint32_t)BootLogo::MARK_H * t / LOGO_WIPE_MS);
    if (revealH > 0) {
        canvas.setClipRect(logoX, logoY, BootLogo::MARK_W, revealH);
        canvas.drawBitmap(logoX, logoY, BootLogo::MARK_BITS, BootLogo::MARK_W, BootLogo::MARK_H, Theme::ACCENT);
        canvas.clearClipRect();
    }

    // "CRYPTSPEAK" decrypts into place left-to-right: each letter cycles
    // through noise glyphs briefly before settling, like a key being found.
    canvas.setTextSize(2);
    int titleW = TITLE_LEN * 12;  // size 2 = 12px wide
    int titleX = (Theme::SCREEN_W - titleW) / 2;
    int titleY = 54;
    for (int i = 0; i < TITLE_LEN; i++) {
        uint32_t startI = i * TITLE_STAGGER_MS;
        if (t < startI) break;  // not yet appeared, and none after it have either

        char ch;
        uint16_t color;
        if (t < startI + TITLE_SCRAMBLE_MS) {
            ch = noiseGlyph(i, (t - startI) / 45);
            color = Theme::ACCENT;
        } else {
            ch = TITLE[i];
            color = Theme::PRIMARY;
        }
        canvas.setTextColor(color);
        canvas.setCursor(titleX + i * 12, titleY);
        canvas.print(ch);
    }
    canvas.setTextSize(Theme::FONT_SIZE);

    // Everything below fades in together once the title has settled,
    // instead of popping in at full brightness — a smooth "gradient down"
    // through time rather than a hard cut.
    float revealAmt = 0.0f;
    if (t >= TITLE_DONE_MS) {
        uint32_t dt = t - TITLE_DONE_MS;
        revealAmt = (dt >= REVEAL_FADE_MS) ? 1.0f : (float)dt / REVEAL_FADE_MS;
    }

    if (revealAmt > 0.0f) {
        canvas.setTextColor(Theme::lerp565(Theme::BG, Theme::SECONDARY, revealAmt));
        const char* subtitle = "RSCARDPUTER-CE";
        int subW = strlen(subtitle) * Theme::CHAR_W;
        canvas.setCursor((Theme::SCREEN_W - subW) / 2, 76);
        canvas.print(subtitle);

        char verStr[32];
        snprintf(verStr, sizeof(verStr), "v%s", RSCARDPUTER_VERSION_STRING);
        canvas.setTextColor(Theme::lerp565(Theme::BG, Theme::MUTED, revealAmt));
        int verW = strlen(verStr) * Theme::CHAR_W;
        canvas.setCursor((Theme::SCREEN_W - verW) / 2, 90);
        canvas.print(verStr);
    }

    // Progress bar (near bottom), fading in with the same timing.
    int barY = Theme::SCREEN_H - 32;
    _progressBar.render(canvas, 30, barY, Theme::SCREEN_W - 60, 8, revealAmt);

    // Status text below progress bar, terminal-prompt style.
    if (revealAmt > 0.0f) {
        canvas.setTextColor(Theme::lerp565(Theme::BG, Theme::SECONDARY, revealAmt));
        char statusLine[40];
        snprintf(statusLine, sizeof(statusLine), "> %s", _statusText);
        int statusLen = strlen(statusLine) * Theme::CHAR_W;
        canvas.setCursor((Theme::SCREEN_W - statusLen) / 2, barY + 12);
        canvas.print(statusLine);
    }
}
