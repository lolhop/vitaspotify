#include "Gfx.h"

#include "Config.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <psp2/sysmodule.h>

#include <vita2d.h>

namespace Gfx {

namespace {
vita2d_pgf *g_font = nullptr;
vita2d_texture *g_art = nullptr;
vita2d_texture *g_ui_icons[static_cast<int>(UiIcon::Count)] = {};
bool g_ready = false;

struct UiIconFile {
    const char *name;
};

constexpr UiIconFile kUiIconFiles[] = {
    {"back.png"},
    {"forwards.png"},
    {"pauseplay.png"},
    {"repeat.png"},
    {"shuffle.png"},
};

void freeUiIcons() {
    for (auto *&tex : g_ui_icons) {
        if (tex) {
            vita2d_free_texture(tex);
            tex = nullptr;
        }
    }
}

void loadUiIcons() {
    for (int i = 0; i < static_cast<int>(UiIcon::Count); i++) {
        char path[96];
        snprintf(path, sizeof(path), "%s%s", UI_ASSETS_PREFIX,
                 kUiIconFiles[i].name);
        g_ui_icons[i] = vita2d_load_PNG_file(path);
    }
}

// vita2d PGF draws relative to a baseline; approximate cap/line metrics so
// callers can pass a comfortable top-left y.
constexpr float kBaseGlyphPx = 20.0f;
}  // namespace

bool init() {
    if (g_ready) {
        return true;
    }
    vita2d_init();
    vita2d_set_clear_color(COLOR_BG);
    g_font = vita2d_load_default_pgf();
    loadUiIcons();
    g_ready = true;
    return true;
}

void fini() {
    if (!g_ready) {
        return;
    }
    if (g_art) {
        vita2d_free_texture(g_art);
        g_art = nullptr;
    }
    freeUiIcons();
    if (g_font) {
        vita2d_free_pgf(g_font);
        g_font = nullptr;
    }
    vita2d_fini();
    g_ready = false;
}

void beginFrame() {
    vita2d_start_drawing();
    vita2d_clear_screen();
}

void clear(uint32_t color) {
    vita2d_set_clear_color(color);
    vita2d_clear_screen();
}

void endFrame() {
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void presentDialogFrame() {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

void clearAlbumArt() {
    if (g_art) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(g_art);
        g_art = nullptr;
    }
}

void setAlbumArtJpeg(const void *data, unsigned long len) {
    clearAlbumArt();
    if (data && len > 0) {
        g_art = vita2d_load_JPEG_buffer(data, len);
    }
}

bool setAlbumArtFromFile(const char *path) {
    clearAlbumArt();
    if (!path || !path[0]) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    const long sz = ftell(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    rewind(f);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (fread(buf.data(), 1, static_cast<size_t>(sz), f) !=
        static_cast<size_t>(sz)) {
        fclose(f);
        return false;
    }
    fclose(f);
    g_art = vita2d_load_JPEG_buffer(buf.data(), static_cast<unsigned long>(sz));
    return g_art != nullptr;
}

bool hasUiIcon(UiIcon icon) {
    const int i = static_cast<int>(icon);
    return i >= 0 && i < static_cast<int>(UiIcon::Count) && g_ui_icons[i] != nullptr;
}

void drawUiIcon(UiIcon icon, float x, float y, float size, bool flipX,
                uint32_t tint) {
    const int i = static_cast<int>(icon);
    if (i < 0 || i >= static_cast<int>(UiIcon::Count)) {
        return;
    }
    vita2d_texture *tex = g_ui_icons[i];
    if (!tex) {
        return;
    }
    const unsigned int tw = vita2d_texture_get_width(tex);
    const unsigned int th = vita2d_texture_get_height(tex);
    if (tw == 0 || th == 0) {
        return;
    }
    const float scale = size / static_cast<float>(tw > th ? tw : th);
    const float sx = flipX ? -scale : scale;
    float drawX = x;
    if (flipX) {
        drawX = x + size;
    }
    vita2d_draw_texture_tint_scale(tex, drawX, y + (size - th * scale) / 2.0f, sx,
                                   scale, tint);
}

bool drawAlbumArt(float x, float y, float size) {
    if (!g_art) {
        return false;
    }
    const unsigned int tw = vita2d_texture_get_width(g_art);
    const unsigned int th = vita2d_texture_get_height(g_art);
    if (tw == 0 || th == 0) {
        return false;
    }
    const float scale = size / static_cast<float>(tw > th ? tw : th);
    // Center within the square if not perfectly square.
    const float dx = x + (size - tw * scale) / 2.0f;
    const float dy = y + (size - th * scale) / 2.0f;
    vita2d_draw_texture_scale(g_art, dx, dy, scale, scale);
    return true;
}

void rect(float x, float y, float w, float h, uint32_t color) {
    vita2d_draw_rectangle(x, y, w, h, color);
}

void rectOutline(float x, float y, float w, float h, float t, uint32_t color) {
    vita2d_draw_rectangle(x, y, w, t, color);
    vita2d_draw_rectangle(x, y + h - t, w, t, color);
    vita2d_draw_rectangle(x, y, t, h, color);
    vita2d_draw_rectangle(x + w - t, y, t, h, color);
}

void circle(float x, float y, float radius, uint32_t color) {
    vita2d_draw_fill_circle(x, y, radius, color);
}

void text(float x, float y, float scale, uint32_t color, const char *str) {
    if (!g_font || !str) {
        return;
    }
    // Shift baseline down so y is the visual top of the text.
    const int baseline = static_cast<int>(y + kBaseGlyphPx * scale);
    vita2d_pgf_draw_text(g_font, static_cast<int>(x), baseline, color, scale,
                         str);
}

void textf(float x, float y, float scale, uint32_t color, const char *fmt,
           ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    text(x, y, scale, color, buf);
}

int textWidth(float scale, const char *str) {
    if (!g_font || !str) {
        return 0;
    }
    return vita2d_pgf_text_width(g_font, scale, str);
}

int textHeight(float scale) {
    return static_cast<int>(kBaseGlyphPx * scale * 1.4f);
}

void drawVitaBackground() {
    constexpr int bands = 24;
    const float bandH = static_cast<float>(SCREEN_H) / bands;
    for (int i = 0; i < bands; i++) {
        const float t = static_cast<float>(i) / (bands - 1);
        const uint8_t r =
            static_cast<uint8_t>(0x3A + t * (0x6A - 0x3A));
        const uint8_t g =
            static_cast<uint8_t>(0x10 + t * (0x28 - 0x10));
        const uint8_t b =
            static_cast<uint8_t>(0x5C + t * (0x8C - 0x5C));
        const uint32_t col = 0xFF000000u | (r << 16) | (g << 8) | b;
        rect(0, i * bandH, SCREEN_W, bandH + 1, col);
    }
}

void drawProgressBar(float x, float y, float w, float h, float progress) {
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    if (progress > 1.0f) {
        progress = 1.0f;
    }
    rect(x, y, w, h, COLOR_PANEL);
    if (progress > 0.0f) {
        rect(x, y, w * progress, h, COLOR_ACCENT);
    }
    const float knobX = x + w * progress;
    circle(knobX, y + h / 2.0f, h * 0.85f, COLOR_TEXT);
}

void drawPillButton(float x, float y, float w, float h, uint32_t fill,
                    uint32_t border, bool highlighted) {
    const float r = h / 2.0f;
    rect(x + r, y, w - 2 * r, h, fill);
    circle(x + r, y + r, r, fill);
    circle(x + w - r, y + r, r, fill);
    if (highlighted) {
        rectOutline(x + 2, y + 2, w - 4, h - 4, 2, COLOR_VITA_HI);
    } else if (border) {
        rectOutline(x, y, w, h, 2, border);
    }
}

void textClipped(float x, float y, float scale, uint32_t color,
                 const char *str, int max_w) {
    if (!g_font || !str) {
        return;
    }
    if (textWidth(scale, str) <= max_w) {
        text(x, y, scale, color, str);
        return;
    }

    std::string s(str);
    const std::string ellipsis = "...";
    while (!s.empty()) {
        std::string candidate = s + ellipsis;
        if (textWidth(scale, candidate.c_str()) <= max_w) {
            text(x, y, scale, color, candidate.c_str());
            return;
        }
        s.pop_back();
    }
}

}  // namespace Gfx
