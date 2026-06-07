#pragma once

#include <cstdint>

// Thin wrapper around vita2d for a consistent, themed UI.
namespace Gfx {

constexpr int SCREEN_W = 960;
constexpr int SCREEN_H = 544;

// Theme palette (Spotify-like dark).
constexpr uint32_t COLOR_BG = 0xFF161616;        // near-black background
constexpr uint32_t COLOR_PANEL = 0xFF222226;     // raised panel
constexpr uint32_t COLOR_PANEL_HI = 0xFF31313A;  // selected row
constexpr uint32_t COLOR_ACCENT = 0xFF54B91D;    // Spotify green (RGBA8 r,g,b)
constexpr uint32_t COLOR_ACCENT_DIM = 0xFF2E7D14;
constexpr uint32_t COLOR_TEXT = 0xFFFFFFFF;
constexpr uint32_t COLOR_SUBTEXT = 0xFFAAAAAA;
constexpr uint32_t COLOR_MUTED = 0xFF6E6E6E;
constexpr uint32_t COLOR_VITA_HI = 0xFFB8E986;   // lime highlight (Vita Music)
constexpr uint32_t COLOR_VITA_PANEL = 0xAA5C2D91;  // translucent purple button

bool init();
void fini();

void beginFrame();
void clear(uint32_t color = COLOR_BG);
void endFrame();

// Renders a single frame that composites any active system common dialog
// (used by the IME keyboard). Returns immediately.
void presentDialogFrame();

// Album art: decodes a JPEG buffer into the single album-art texture, replacing
// any previous one. Pass null/0 to clear. Safe to call from the render thread.
void setAlbumArtJpeg(const void *data, unsigned long len);
bool setAlbumArtFromFile(const char *path);
void clearAlbumArt();
// Draws the album art scaled into a size x size square at (x,y). Returns false
// if no art is loaded.
bool drawAlbumArt(float x, float y, float size);

// Playback / navigation PNGs from app0:/assets/ui/ (see CMakeLists FILE list).
enum class UiIcon : int {
    Back = 0,
    Forwards,
    PausePlay,
    Repeat,
    Shuffle,
    Count,
};

bool hasUiIcon(UiIcon icon);
// Draw icon into a size×size box at (x,y). flipX mirrors horizontally (e.g. prev).
void drawUiIcon(UiIcon icon, float x, float y, float size, bool flipX,
                uint32_t tint = 0xFFFFFFFF);

void rect(float x, float y, float w, float h, uint32_t color);
void rectOutline(float x, float y, float w, float h, float thickness,
                 uint32_t color);
void circle(float x, float y, float radius, uint32_t color);

// Draws text with the loaded font. scale ~1.0 is roughly an 18px cap height.
void text(float x, float y, float scale, uint32_t color, const char *str);
void textf(float x, float y, float scale, uint32_t color, const char *fmt, ...);
int textWidth(float scale, const char *str);
int textHeight(float scale);

// Draws text clipped/truncated with an ellipsis to fit max_w pixels.
void textClipped(float x, float y, float scale, uint32_t color,
                 const char *str, int max_w);

// Vita Music-style purple gradient background.
void drawVitaBackground();

// Filled bar with optional scrub knob (0..1 progress).
void drawProgressBar(float x, float y, float w, float h, float progress);

// Glossy rounded control (radius via corner circles + rect).
void drawPillButton(float x, float y, float w, float h, uint32_t fill,
                    uint32_t border, bool highlighted);

}  // namespace Gfx
