#include "App.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include <CDNAudioFile.h>
#include <LoginBlob.h>
#include <SpircHandler.h>
#include <TrackQueue.h>

#include "CliFile.h"
#include "Config.h"
#include "Gfx.h"
#include "Keyboard.h"
#include "Touch.h"
#include "Log.h"
#include "LocalPlayer.h"
#include "OfflineStore.h"
#include "VitaAudioSink.h"
#include "VitaPlayer.h"
#include "Utils.h"

extern std::shared_ptr<CliFile> g_file;
extern std::shared_ptr<cspot::LoginBlob> g_loginBlob;
extern std::shared_ptr<cspot::SpircHandler> g_spircHandler;
extern std::shared_ptr<class VitaPlayer> g_player;
extern std::atomic<bool> g_playbackPaused;
extern std::mutex g_cspot_mutex;

#ifndef VITA_APP_VERSION
#define VITA_APP_VERSION "?"
#endif

void login_cspot(const char *user, const char *password);

static constexpr int BROWSE_MENU_COUNT = 5;
static constexpr int DOWNLOADS_PAGE_SIZE = 10;
static constexpr int RESULTS_PAGE_SIZE = 10;
static constexpr int PLAYLISTS_PAGE_SIZE = 10;

// Now Playing focus order (matches D-pad grid).
enum PlaybackFocus {
    FocusLibrary = 0,
    FocusSeek = 1,
    FocusPrev = 2,
    FocusPlayPause = 3,
    FocusNext = 4,
    FocusShuffle = 5,
    FocusRepeat = 6,
    FocusCount = 7,
};

static int playback_ui_from_focus(int focus) {
    switch (focus) {
        case FocusLibrary:
            return static_cast<int>(PlaybackUi::Browse);
        case FocusSeek:
            return static_cast<int>(PlaybackUi::SeekBar);
        case FocusPrev:
            return static_cast<int>(PlaybackUi::Prev);
        case FocusPlayPause:
            return static_cast<int>(PlaybackUi::PlayPause);
        case FocusNext:
            return static_cast<int>(PlaybackUi::Next);
        case FocusShuffle:
            return static_cast<int>(PlaybackUi::Shuffle);
        case FocusRepeat:
            return static_cast<int>(PlaybackUi::Repeat);
        default:
            return static_cast<int>(PlaybackUi::PlayPause);
    }
}

static int playback_focus_from_ui(int uiId) {
    switch (uiId) {
        case static_cast<int>(PlaybackUi::Browse):
            return FocusLibrary;
        case static_cast<int>(PlaybackUi::SeekBar):
            return -1;  // touch-only; never D-pad focusable
        case static_cast<int>(PlaybackUi::Prev):
            return FocusPrev;
        case static_cast<int>(PlaybackUi::PlayPause):
            return FocusPlayPause;
        case static_cast<int>(PlaybackUi::Next):
            return FocusNext;
        case static_cast<int>(PlaybackUi::Shuffle):
            return FocusShuffle;
        case static_cast<int>(PlaybackUi::Repeat):
            return FocusRepeat;
        default:
            return -1;
    }
}

// Login menu (auth.json is the only working sign-in path, so username/password
// entry was removed). With a saved session: [Log in], [Play offline], [Quit].
// Without one: [Play offline], [Quit] (drop auth.json in to enable Log in).
static int login_item_count(bool has_saved) {
    return has_saved ? 3 : 2;
}

static int login_authjson_item_index(bool has_saved) {
    return has_saved ? 0 : -1;
}

static int login_offline_item_index(bool has_saved) {
    return has_saved ? 1 : 0;
}

static int login_quit_item_index(bool has_saved) {
    return has_saved ? 2 : 1;
}

void App::set_status(const std::string &msg) {
    std::lock_guard<std::mutex> lock(ui_mutex);
    status_line = msg;
    request_redraw();
}

void App::request_redraw() {
    needs_redraw = true;
}

static bool load_dev_login(char *user, size_t user_len, char *pass, size_t pass_len) {
    if (!g_file) {
        return false;
    }

    std::string content;
    if (!g_file->readFile(DEV_LOGIN_FILE_NAME, content) || content.empty()) {
        return false;
    }

    std::string profile;
    std::string password;
    int line_num = 0;

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t end = content.find('\n', pos);
        if (end == std::string::npos) {
            end = content.size();
        }
        std::string line = content.substr(pos, end - pos);
        pos = end + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            start++;
        }
        if (start > 0) {
            line = line.substr(start);
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
                value.erase(0, 1);
            }
            if (key == "profile_name" || key == "username" || key == "user") {
                profile = value;
            } else if (key == "password" || key == "pass") {
                password = value;
            }
            continue;
        }

        if (line_num == 0) {
            profile = line;
        } else if (line_num == 1) {
            password = line;
        }
        line_num++;
    }

    if (profile.empty() || password.empty()) {
        return false;
    }

    strncpy(user, profile.c_str(), user_len - 1);
    user[user_len - 1] = '\0';
    strncpy(pass, password.c_str(), pass_len - 1);
    pass[pass_len - 1] = '\0';
    return true;
}

// --- Shared UI building blocks -------------------------------------------

static constexpr float kHeaderH = 64.0f;
static constexpr float kHeaderLineH = 2.0f;
static constexpr float kContentTop = kHeaderH + kHeaderLineH + 18.0f;

static void draw_header_bar() {
    Gfx::rect(0, 0, Gfx::SCREEN_W, kHeaderH, Gfx::COLOR_PANEL);
    Gfx::rect(0, kHeaderH, Gfx::SCREEN_W, kHeaderLineH, Gfx::COLOR_ACCENT);
}

// slot 0 is the rightmost button; higher slots sit to its left.
static void draw_header_nav_button(App *app, const char *label, bool highlighted,
                                   int hitId, int slot = 0) {
    constexpr float btnW = 148.0f;
    constexpr float btnH = 40.0f;
    constexpr float gap = 12.0f;
    const float btnX =
        Gfx::SCREEN_W - 20.0f - btnW - static_cast<float>(slot) * (btnW + gap);
    const float btnY = 12.0f;
    const uint32_t fill =
        highlighted ? Gfx::COLOR_PANEL_HI : Gfx::COLOR_PANEL;
    Gfx::rect(btnX, btnY, btnW, btnH, fill);
    if (highlighted) {
        Gfx::rectOutline(btnX, btnY, btnW, btnH, 2, Gfx::COLOR_ACCENT);
    } else {
        Gfx::rectOutline(btnX, btnY, btnW, btnH, 1, Gfx::COLOR_MUTED);
    }
    const int tw = Gfx::textWidth(0.82f, label);
    Gfx::text(btnX + (btnW - tw) / 2, btnY + 10, 0.82f,
              highlighted ? Gfx::COLOR_TEXT : Gfx::COLOR_ACCENT, label);
    if (app) {
        app->add_ui_hit(btnX, btnY, btnW, btnH, hitId);
    }
}

static void draw_screen_header(App *app, const char *title, bool showBack) {
    draw_header_bar();
    Gfx::text(24, 18, 1.15f, Gfx::COLOR_TEXT, title);
    if (showBack && app) {
        draw_header_nav_button(app, "<  Back", false, UI_NAV_BACK, 0);
    }
}

static void draw_footer(const char *hint) {
    Gfx::rect(0, Gfx::SCREEN_H - 40, Gfx::SCREEN_W, 40, Gfx::COLOR_PANEL);
    Gfx::text(24, Gfx::SCREEN_H - 33, 0.75f, Gfx::COLOR_MUTED, hint);
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", VITA_APP_VERSION);
    const int vw = Gfx::textWidth(0.72f, ver);
    Gfx::text(Gfx::SCREEN_W - vw - 20, Gfx::SCREEN_H - 33, 0.72f,
              Gfx::COLOR_MUTED, ver);
}

// Now Playing header: Library control plus (when online) an Offline button.
static void draw_playback_header(App *app, bool libraryFocused, bool online) {
    draw_header_bar();
    Gfx::text(24, 18, 1.15f, Gfx::COLOR_TEXT, "Now Playing");
    draw_header_nav_button(app, "Library  >", libraryFocused,
                           static_cast<int>(PlaybackUi::Browse), 0);
    if (online) {
        draw_header_nav_button(app, "Go offline", false, UI_NAV_OFFLINE, 1);
    }
}

static void draw_transport_button(App *app, float x, float y, float w, float h,
                                  Gfx::UiIcon icon, bool flipX,
                                  const char *fallbackLabel, bool primary,
                                  bool focused, int hitId) {
    uint32_t fill = Gfx::COLOR_PANEL;
    if (primary) {
        fill = focused ? Gfx::COLOR_ACCENT : Gfx::COLOR_PANEL_HI;
    } else if (focused) {
        fill = Gfx::COLOR_PANEL_HI;
    }
    const uint32_t iconCol =
        (primary && focused) ? 0xFF101010 : Gfx::COLOR_TEXT;
    Gfx::rect(x, y, w, h, fill);
    if (focused) {
        Gfx::rectOutline(x, y, w, h, 2, Gfx::COLOR_ACCENT);
    } else if (!primary) {
        Gfx::rectOutline(x, y, w, h, 1, Gfx::COLOR_PANEL_HI);
    }
    constexpr float kIconSz = 44.0f;
    if (Gfx::hasUiIcon(icon)) {
        Gfx::drawUiIcon(icon, x + (w - kIconSz) / 2.0f, y + (h - kIconSz) / 2.0f,
                        kIconSz, flipX, iconCol);
    } else if (fallbackLabel) {
        const int tw = Gfx::textWidth(1.0f, fallbackLabel);
        Gfx::text(x + (w - tw) / 2, y + (h - Gfx::textHeight(1.0f)) / 2 + 4,
                  1.0f, iconCol, fallbackLabel);
    }
    app->add_ui_hit(x, y, w, h, hitId);
}

static void draw_mode_toggle(App *app, float x, float y, float w, float h,
                             Gfx::UiIcon icon, const char *fallbackLabel,
                             bool on, bool focused, int hitId) {
    uint32_t fill = on ? Gfx::COLOR_PANEL_HI : Gfx::COLOR_PANEL;
    if (focused) {
        fill = Gfx::COLOR_PANEL_HI;
    }
    Gfx::rect(x, y, w, h, fill);
    const uint32_t stripe = on ? Gfx::COLOR_ACCENT : Gfx::COLOR_PANEL_HI;
    Gfx::rect(x, y, focused ? 6 : 4, h, stripe);
    if (focused) {
        Gfx::rectOutline(x, y, w, h, 2, Gfx::COLOR_ACCENT);
    }
    const uint32_t iconCol =
        on || focused ? Gfx::COLOR_TEXT : Gfx::COLOR_SUBTEXT;
    constexpr float kIconSz = 36.0f;
    const float stripeW = focused ? 6.0f : 4.0f;
    if (Gfx::hasUiIcon(icon)) {
        Gfx::drawUiIcon(icon, x + stripeW + (w - stripeW - kIconSz) / 2.0f,
                        y + (h - kIconSz) / 2.0f, kIconSz, false, iconCol);
    } else if (fallbackLabel) {
        Gfx::text(x + 16, y + 14, 0.88f, iconCol, fallbackLabel);
    }
    app->add_ui_hit(x, y, w, h, hitId);
}

// A selectable list row. y is the row top; returns next row y.
static float draw_row(float y, bool selected, const char *primary,
                      const char *secondary) {
    constexpr float kRowH = 50.0f;
    const float x = 24.0f;
    const float w = Gfx::SCREEN_W - 48.0f;
    if (selected) {
        Gfx::rect(x, y, w, kRowH, Gfx::COLOR_PANEL_HI);
        Gfx::rect(x, y, 4, kRowH, Gfx::COLOR_ACCENT);
    }
    const uint32_t titleColor = selected ? Gfx::COLOR_TEXT : Gfx::COLOR_SUBTEXT;
    if (secondary && secondary[0] != '\0') {
        Gfx::textClipped(x + 20, y + 6, 0.95f, titleColor, primary, (int)w - 40);
        Gfx::textClipped(x + 20, y + 28, 0.7f, Gfx::COLOR_MUTED, secondary,
                         (int)w - 40);
    } else {
        Gfx::textClipped(x + 20, y + 14, 1.0f, titleColor, primary, (int)w - 40);
    }
    return y + kRowH + 4;
}

static void format_time_ms(uint32_t ms, char *buf, size_t len) {
    const uint32_t sec = ms / 1000;
    snprintf(buf, len, "%02u:%02u", sec / 60, sec % 60);
}

void App::clear_ui_hits() {
    ui_hits.clear();
}

void App::add_ui_hit(float x, float y, float w, float h, int id) {
    ui_hits.push_back({x, y, w, h, id});
}

int App::hit_test(float x, float y) const {
    for (int i = static_cast<int>(ui_hits.size()) - 1; i >= 0; i--) {
        const UiHit &h = ui_hits[i];
        if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) {
            return h.id;
        }
    }
    return -1;
}

void App::queue_transport(TransportCmd cmd) {
    int expected = static_cast<int>(TransportCmd::None);
    pending_transport.compare_exchange_strong(
        expected, static_cast<int>(cmd));
}

void App::playback_action_prev() {
    queue_transport(TransportCmd::Prev);
    request_redraw();
}

void App::playback_action_next() {
    queue_transport(TransportCmd::Next);
    request_redraw();
}

void App::playback_action_toggle() {
    if (local_playback.load()) {
        if (auto lp = vitaspotify_get_local_player()) {
            const bool p = !lp->isPaused();
            lp->setPaused(p);
            is_paused = p;
            request_redraw();
        }
        return;
    }
    queue_transport(TransportCmd::TogglePlay);
    request_redraw();
}

void App::playback_action_shuffle() {
    if (local_playback.load()) {
        local_shuffle_on = !local_shuffle_on;
        shuffle_on = local_shuffle_on;
        request_redraw();
        return;
    }
    queue_transport(TransportCmd::ToggleShuffle);
    request_redraw();
}

void App::playback_action_repeat() {
    if (local_playback.load()) {
        local_repeat_on = !local_repeat_on;
        repeat_on = local_repeat_on;
        request_redraw();
        return;
    }
    queue_transport(TransportCmd::ToggleRepeat);
    request_redraw();
}

void App::playback_action_browse() {
    if (local_playback.load() || offline_library_mode) {
        // Offline: Library goes to the home menu (All Songs + playlists).
        OfflineStore::listPlaylists(offline_playlists);
        screen = Screen::OFFLINE_HOME;
        request_redraw();
        return;
    }
    screen = Screen::BROWSE;
    browse_index = 0;
    request_redraw();
}

void App::request_download_current_track() {
    if (download_in_progress.load()) {
        set_status("Download already in progress...");
        return;
    }
    if (!cspot_started) {
        set_status("Connect to Spotify first.");
        return;
    }
    pending_download = true;
    set_status("Starting download...");
    request_redraw();
}

void App::on_download_finished(bool ok, const std::string& status) {
    download_in_progress = false;
    set_status(status);
    if (ok) {
        current_track_downloaded = true;
        refresh_current_track_downloaded();
    }
    if (screen == Screen::DOWNLOADS) {
        refresh_downloads_list();
    }
    request_redraw();
}

void App::stop_offline_playback() {
    local_playback = false;
    offline_play_index = -1;
    if (auto lp = vitaspotify_get_local_player()) {
        lp->stopAndClear();
    }
}

void App::refresh_downloads_list() {
    downloads.clear();
    if (current_category_all) {
        OfflineStore::listAllTracks(downloads);
    } else {
        OfflineStore::listTracks(downloads, current_category_playlist);
    }
}

void App::enter_offline_library() {
    vitaspotify_ensure_local_audio();
    offline_library_mode = true;
    auth_failed = false;
    auth_failure_msg[0] = '\0';
    OfflineStore::listPlaylists(offline_playlists);
    offline_home_index = 0;
    screen = Screen::OFFLINE_HOME;
    set_status("");
    request_redraw();
}

void App::enter_offline_mode_from_online() {
    request_disconnect = true;
    cspot_started = false;
    is_paused = true;
    vitaspotify_halt_online_playback();
    vitaspotify_stop_network_thread();
    enter_offline_library();
}

void App::open_offline_category(int index) {
    if (index <= 0) {
        current_category_all = true;
        current_category_playlist.clear();
    } else {
        current_category_all = false;
        const int pl = index - 1;
        if (pl < 0 || pl >= static_cast<int>(offline_playlists.size())) {
            return;
        }
        current_category_playlist = offline_playlists[pl];
    }
    dl_index = 0;
    dl_page = 0;
    refresh_downloads_list();
    downloads_return_screen = Screen::OFFLINE_HOME;
    screen = Screen::DOWNLOADS;
    set_status(downloads.empty() ? "No downloads here yet." : "");
    request_redraw();
}

void App::offline_home_activate_row(int row) {
    open_offline_category(row);
}

void App::refresh_current_track_downloaded() {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        name = track_name;
    }
    current_track_downloaded = !name.empty() && name != "No track" &&
                               OfflineStore::hasDownloadAnywhere(name);
}

void App::handle_local_track_ended() {
    if (!local_playback.load()) {
        return;
    }
    if (downloads.empty()) {
        refresh_downloads_list();
    }
    if (local_repeat_on) {
        if (offline_play_index >= 0 &&
            offline_play_index < static_cast<int>(downloads.size())) {
            play_offline_file(downloads[static_cast<size_t>(offline_play_index)].path);
        }
        return;
    }
    play_offline_relative(1);
}

void App::refresh_local_playback_state() {
    if (!local_playback.load()) {
        return;
    }
    auto lp = vitaspotify_get_local_player();
    if (!lp) {
        return;
    }
    if (lp->consumeTrackEnded()) {
        handle_local_track_ended();
        return;
    }
    if (!lp->isActive()) {
        return;
    }
    track_position_ms.store(lp->getPositionMs());
    const uint32_t dur = lp->getDurationMs();
    if (dur > 0) {
        track_duration_ms.store(dur);
    }
    is_paused = lp->isPaused();
    shuffle_on = local_shuffle_on;
    repeat_on = local_repeat_on;
}

void App::play_offline_relative(int delta) {
    if (downloads.empty()) {
        refresh_downloads_list();
    }
    if (downloads.empty()) {
        return;
    }
    const int total = static_cast<int>(downloads.size());
    int idx = offline_play_index;
    if (idx < 0 || idx >= total) {
        idx = 0;
    }
    if (local_shuffle_on && total > 1 && delta != 0) {
        int next = idx;
        for (int tries = 0; tries < 8 && next == idx; tries++) {
            next = static_cast<int>(rand() % total);
        }
        idx = next;
    } else {
        idx = (idx + delta + total) % total;
    }
    play_offline_file(downloads[static_cast<size_t>(idx)].path);
}

void App::play_offline_file(const std::string& oggPath) {
    vitaspotify_ensure_local_audio();
    auto lp = vitaspotify_get_local_player();
    if (!lp) {
        set_status("Offline player not ready.");
        return;
    }

    std::string title;
    std::string artist;
    OfflineStore::readMeta(oggPath, title, artist);
    if (title.empty()) {
        title = "Offline track";
    }

    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        track_name = title;
        track_artist = artist;
        track_album = "Offline";
        track_image_url.clear();
    }
    offline_art_path = OfflineStore::coverPathForOgg(oggPath);
    loaded_art_url.clear();
    track_dirty = true;
    track_duration_ms = 0;
    track_position_ms = 0;

    {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_spircHandler) {
            g_spircHandler->setPause(true);
        }
        if (g_player) {
            g_player->onPlayPause(true);
            g_player->onFlush();
        }
        g_playbackPaused = true;
    }
    is_paused = false;

    stop_offline_playback();
    local_playback = true;
    if (auto sink = vitaspotify_get_audio_sink()) {
        sink->clearBufferedAudio();
    }
    lp->play(oggPath);

    if (downloads.empty()) {
        refresh_downloads_list();
    }
    offline_play_index = -1;
    for (size_t i = 0; i < downloads.size(); i++) {
        if (downloads[i].path == oggPath) {
            offline_play_index = static_cast<int>(i);
            dl_page = offline_play_index / DOWNLOADS_PAGE_SIZE;
            dl_index = offline_play_index - dl_page * DOWNLOADS_PAGE_SIZE;
            break;
        }
    }

    offline_library_mode = false;
    downloads_return_screen = Screen::PLAYBACK;
    shuffle_on = local_shuffle_on;
    repeat_on = local_repeat_on;
    screen = Screen::PLAYBACK;
    playback_focus = FocusPlayPause;
    set_status("Playing offline: " + title);
    request_redraw();
}

void App::open_downloads() {
    open_downloads(Screen::BROWSE);
}

void App::open_downloads(Screen returnTo) {
    downloads_return_screen = returnTo;
    dl_index = 0;
    dl_page = 0;
    current_category_all = true;
    current_category_playlist.clear();
    refresh_downloads_list();
    screen = Screen::DOWNLOADS;
    set_status(downloads.empty() ? "No downloads yet." : "");
    request_redraw();
}

void App::navigate_back() {
    switch (screen) {
        case Screen::BROWSE:
            screen = Screen::PLAYBACK;
            break;
        case Screen::PLAYLISTS:
            screen = Screen::BROWSE;
            break;
        case Screen::DOWNLOADS:
            screen = downloads_return_screen;
            if (downloads_return_screen != Screen::LOGIN &&
                downloads_return_screen != Screen::OFFLINE_HOME) {
                offline_library_mode = false;
            }
            break;
        case Screen::OFFLINE_HOME:
            if (cspot_started || local_playback.load()) {
                screen = Screen::PLAYBACK;
            } else {
                offline_library_mode = false;
                screen = Screen::LOGIN;
            }
            break;
        case Screen::RESULTS:
            screen = results_back;
            break;
        default:
            break;
    }
    request_redraw();
}

void App::move_playback_focus(int dx, int dy) {
    int f = playback_focus;
    if (dx < 0) {
        if (f == FocusPlayPause) {
            f = FocusPrev;
        } else if (f == FocusNext) {
            f = FocusPlayPause;
        } else if (f == FocusRepeat) {
            f = FocusShuffle;
        }
    } else if (dx > 0) {
        if (f == FocusPrev) {
            f = FocusPlayPause;
        } else if (f == FocusPlayPause) {
            f = FocusNext;
        } else if (f == FocusShuffle) {
            f = FocusRepeat;
        }
    }
    if (dy < 0) {
        switch (f) {
            case FocusPrev:
            case FocusPlayPause:
            case FocusNext:
                f = FocusLibrary;
                break;
            case FocusShuffle:
            case FocusRepeat:
                f = FocusPlayPause;
                break;
            default:
                break;
        }
    } else if (dy > 0) {
        switch (f) {
            case FocusLibrary:
                f = FocusPlayPause;
                break;
            case FocusPrev:
            case FocusPlayPause:
            case FocusNext:
                f = FocusShuffle;
                break;
            default:
                break;
        }
    }
    if (f >= 0 && f < FocusCount) {
        playback_focus = f;
        request_redraw();
    }
}

void App::activate_playback_focus() {
    switch (playback_focus) {
        case FocusLibrary:
            playback_action_browse();
            break;
        case FocusSeek:
            break;
        case FocusPrev:
            playback_action_prev();
            break;
        case FocusPlayPause:
            playback_action_toggle();
            break;
        case FocusNext:
            playback_action_next();
            break;
        case FocusShuffle:
            playback_action_shuffle();
            break;
        case FocusRepeat:
            playback_action_repeat();
            break;
        default:
            break;
    }
}

void App::set_playback_focus_from_ui(int uiId) {
    const int f = playback_focus_from_ui(uiId);
    if (f >= 0) {
        playback_focus = f;
    }
}

void App::block_pad_confirm_after_touch(unsigned ms) {
    pad_confirm_blocked_until_us =
        sceKernelGetProcessTimeWide() + static_cast<uint64_t>(ms) * 1000;
}

bool App::is_pad_confirm_blocked() const {
    return sceKernelGetProcessTimeWide() < pad_confirm_blocked_until_us;
}

void App::playback_action_seek_at(float x) {
    if (seek_bar_w <= 0.0f) {
        return;
    }
    float frac = (x - seek_bar_x) / seek_bar_w;
    if (frac < 0.0f) {
        frac = 0.0f;
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }
    const uint32_t dur = track_duration_ms.load();
    if (dur == 0) {
        return;
    }
    pending_seek_ms.store(static_cast<uint32_t>(frac * static_cast<float>(dur)));
    pending_seek.store(true);
    request_redraw();
}

void App::process_touch() {
    float x = 0, y = 0;
    if (!Touch::consumeTap(x, y)) {
        return;
    }

    const int id = hit_test(x, y);
    if (id < 0) {
        return;
    }

    if (id == UI_NAV_BACK) {
        block_pad_confirm_after_touch();
        navigate_back();
        return;
    }

    if (id == UI_NAV_ONLINE) {
        block_pad_confirm_after_touch();
        go_online();
        return;
    }

    if (id == UI_NAV_OFFLINE) {
        block_pad_confirm_after_touch();
        enter_offline_mode_from_online();
        return;
    }

    if (id == UI_PL_DOWNLOAD) {
        block_pad_confirm_after_touch();
        request_download_playlist();
        return;
    }

    block_pad_confirm_after_touch();

    switch (screen) {
        case Screen::PLAYBACK:
            set_playback_focus_from_ui(id);
            if (id == static_cast<int>(PlaybackUi::Prev)) {
                playback_action_prev();
            } else if (id == static_cast<int>(PlaybackUi::PlayPause)) {
                playback_action_toggle();
            } else if (id == static_cast<int>(PlaybackUi::Next)) {
                playback_action_next();
            } else if (id == static_cast<int>(PlaybackUi::Shuffle)) {
                playback_action_shuffle();
            } else if (id == static_cast<int>(PlaybackUi::Repeat)) {
                playback_action_repeat();
            } else if (id == static_cast<int>(PlaybackUi::Browse)) {
                playback_action_browse();
            } else if (id == static_cast<int>(PlaybackUi::SeekBar)) {
                playback_action_seek_at(x);
            } else if (id == static_cast<int>(PlaybackUi::Download)) {
                request_download_current_track();
            }
            break;

        case Screen::LOGIN: {
            const int row = id - static_cast<int>(ListUi::RowBase);
            if (row >= 0 && row < login_item_count(has_saved_session)) {
                login_activate_item(row);
            }
            break;
        }

        case Screen::OFFLINE_HOME: {
            const int row = id - static_cast<int>(ListUi::RowBase);
            if (row >= 0 && row < 1 + static_cast<int>(offline_playlists.size())) {
                offline_home_activate_row(row);
            }
            break;
        }

        case Screen::BROWSE: {
            const int row = id - static_cast<int>(ListUi::RowBase);
            if (row >= 0 && row < BROWSE_MENU_COUNT) {
                browse_activate_item(row);
            }
            break;
        }

        case Screen::RESULTS: {
            const int row = id - static_cast<int>(ListUi::RowBase);
            results_activate_row(row);
            break;
        }
        case Screen::PLAYLISTS: {
            const int row = id - static_cast<int>(ListUi::RowBase);
            playlists_activate_row(row);
            break;
        }
        case Screen::DOWNLOADS: {
            const int row = id - static_cast<int>(ListUi::RowBase);
            downloads_activate_row(row);
            break;
        }

        default:
            break;
    }
}

void App::init() {
    srand(static_cast<unsigned>(sceKernelGetProcessTimeWide()));
    Keyboard::initSystem();
    Gfx::init();
    Touch::init();

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    std::string authData;
    has_saved_session =
        g_file && g_file->readFile(CREDENTIALS_FILE_NAME, authData) && !authData.empty();

    if (load_dev_login(username, sizeof(username), password, sizeof(password))) {
        set_status("Dev login loaded from dev_login.txt — Cross on Log in");
    } else if (has_saved_session) {
        set_status("Resume = saved login. Path: ux0:data/vitaspotify/auth.json");
    } else {
        set_status("Cross = select. Put auth.json in ux0:data/vitaspotify/");
    }
}

void App::draw_login() {
    draw_screen_header(this, "VitaSpotify", false);

    const int count = login_item_count(has_saved_session);
    const char *labels[3];
    int n = 0;
    if (has_saved_session) {
        labels[n++] = "Log in (auth.json)";
    }
    labels[n++] = "Play offline (downloads)";
    labels[n++] = "Quit";

    float y = kContentTop;
    for (int i = 0; i < count; i++) {
        const float rowTop = y;
        y = draw_row(y, i == menu_index, labels[i], nullptr);
        add_ui_hit(24, rowTop, Gfx::SCREEN_W - 48, y - rowTop,
                   static_cast<int>(ListUi::RowBase) + i);
    }

    y += 16;
    if (cspot_connecting) {
        Gfx::text(24, y, 0.95f, Gfx::COLOR_ACCENT, "Connecting to Spotify...");
    } else if (auth_failed) {
        Gfx::text(24, y, 0.9f, 0xFF5050E0,
                  auth_failure_msg[0] ? auth_failure_msg : "Login failed.");
        Gfx::text(24, y + 26, 0.8f, Gfx::COLOR_MUTED,
                  "Put auth.json in ux0:data/vitaspotify/ to log in.");
    } else if (!has_saved_session) {
        Gfx::text(24, y, 0.85f, Gfx::COLOR_MUTED,
                  "No auth.json found. Copy it to ux0:data/vitaspotify/ to log in.");
    }
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        if (!status_line.empty()) {
            Gfx::textClipped(24, y + 54, 0.8f, Gfx::COLOR_SUBTEXT,
                             status_line.c_str(), Gfx::SCREEN_W - 48);
        }
    }
    draw_footer("Touch or D-pad   Cross = select   Profile name is not your email");
}

void App::refresh_playback_transport_state() {
    if (!cspot_started) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_cspot_mutex);
    if (!g_spircHandler) {
        return;
    }
    track_position_ms.store(g_spircHandler->getPositionMs());
    shuffle_on.store(g_spircHandler->isShuffleOn());
    repeat_on.store(g_spircHandler->isRepeatOn());
    cspot::TrackInfo info;
    if (g_spircHandler->getTrackQueue()->peekHeadTrackInfo(info) &&
        info.duration > 0) {
        track_duration_ms.store(info.duration);
    }
}

void App::draw_playback() {
    std::string name;
    std::string artist;
    std::string album;
    std::string status;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        name = track_name;
        artist = track_artist;
        album = track_album;
        status = status_line;
    }

    const bool focusLibrary = (playback_focus == FocusLibrary);
    const bool focusPrev = (playback_focus == FocusPrev);
    const bool focusPlay = (playback_focus == FocusPlayPause);
    const bool focusNext = (playback_focus == FocusNext);
    const bool focusShuffle = (playback_focus == FocusShuffle);
    const bool focusRepeat = (playback_focus == FocusRepeat);

    draw_playback_header(this, focusLibrary, cspot_started && !local_playback);

    const bool paused = is_paused.load();
    const bool loading = name.empty() || name == "No track";
    const bool shuffle = shuffle_on.load();
    const bool repeat = repeat_on.load();
    const uint32_t posMs = track_position_ms.load();
    const uint32_t durMs = track_duration_ms.load();
    float progress = 0.0f;
    if (durMs > 0) {
        progress = static_cast<float>(posMs) / static_cast<float>(durMs);
    }

    // Album art — left column.
    const float artX = 24.0f;
    const float artY = 84.0f;
    const float artSz = 368.0f;
    Gfx::rect(artX, artY, artSz, artSz, Gfx::COLOR_PANEL);
    Gfx::rectOutline(artX, artY, artSz, artSz, 2, Gfx::COLOR_PANEL_HI);
    if (!Gfx::drawAlbumArt(artX + 4, artY + 4, artSz - 8)) {
        Gfx::circle(artX + artSz / 2, artY + artSz / 2, 36,
                    Gfx::COLOR_ACCENT_DIM);
        Gfx::circle(artX + artSz / 2 + 12, artY + artSz / 2 - 8, 12,
                    Gfx::COLOR_ACCENT);
    }

    // Track info and controls — right column.
    const float rx = artX + artSz + 32.0f;
    const float rw = Gfx::SCREEN_W - rx - 24.0f;

    Gfx::text(rx, artY + 4, 0.72f, Gfx::COLOR_MUTED, "ARTIST");
    if (!artist.empty()) {
        Gfx::textClipped(rx, artY + 24, 1.05f, Gfx::COLOR_TEXT, artist.c_str(),
                         (int)rw);
    }

    Gfx::text(rx, artY + 58, 0.72f, Gfx::COLOR_MUTED, "TRACK");
    Gfx::textClipped(rx, artY + 78, 1.35f, Gfx::COLOR_TEXT,
                     loading ? "Loading..." : name.c_str(), (int)rw);

    if (!album.empty()) {
        Gfx::textClipped(rx, artY + 118, 0.82f, Gfx::COLOR_SUBTEXT,
                         album.c_str(), (int)rw);
    }

    const float statusY = artY + 152;
    Gfx::circle(rx + 6, statusY + 10, 5,
                paused ? Gfx::COLOR_MUTED : Gfx::COLOR_ACCENT);
    Gfx::text(rx + 20, statusY, 0.8f, Gfx::COLOR_SUBTEXT,
              paused ? "Paused" : "Playing");

    char timeBuf[16];
    char timeEnd[16];
    format_time_ms(posMs, timeBuf, sizeof(timeBuf));
    format_time_ms(durMs, timeEnd, sizeof(timeEnd));
    char timeLine[40];
    snprintf(timeLine, sizeof(timeLine), "%s / %s", timeBuf, timeEnd);
    const int timeW = Gfx::textWidth(0.8f, timeLine);
    Gfx::text(rx + rw - timeW, statusY, 0.8f, Gfx::COLOR_SUBTEXT, timeLine);

    seek_bar_x = rx;
    seek_bar_y = artY + 188;
    seek_bar_w = rw;
    seek_bar_h = 8;
    Gfx::drawProgressBar(seek_bar_x, seek_bar_y, seek_bar_w, seek_bar_h,
                         progress);
    add_ui_hit(seek_bar_x, seek_bar_y - 14, seek_bar_w, seek_bar_h + 28,
               static_cast<int>(PlaybackUi::SeekBar));

    const float tY = artY + 220;
    const float tH = 64.0f;
    const float tGap = 12.0f;
    const float tW = (rw - 2.0f * tGap) / 3.0f;
    draw_transport_button(this, rx, tY, tW, tH, Gfx::UiIcon::Forwards, true,
                          "|<", false, focusPrev,
                          static_cast<int>(PlaybackUi::Prev));
    draw_transport_button(this, rx + tW + tGap, tY, tW, tH,
                          Gfx::UiIcon::PausePlay, false,
                          paused ? "Play" : "Pause", true, focusPlay,
                          static_cast<int>(PlaybackUi::PlayPause));
    draw_transport_button(this, rx + 2.0f * (tW + tGap), tY, tW, tH,
                          Gfx::UiIcon::Forwards, false, ">|", false, focusNext,
                          static_cast<int>(PlaybackUi::Next));

    const float modeY = tY + tH + 20;
    const float modeH = 48.0f;
    const float modeW = (rw - tGap) / 2.0f;
    draw_mode_toggle(this, rx, modeY, modeW, modeH, Gfx::UiIcon::Shuffle,
                     "Shuffle", shuffle, focusShuffle,
                     static_cast<int>(PlaybackUi::Shuffle));
    draw_mode_toggle(this, rx + modeW + tGap, modeY, modeW, modeH,
                     Gfx::UiIcon::Repeat, "Repeat", repeat, focusRepeat,
                     static_cast<int>(PlaybackUi::Repeat));

    if (cspot_started && !local_playback.load()) {
        constexpr float dlBtnW = 96.0f;
        constexpr float dlBtnH = 28.0f;
        const float dlX = rx + rw - dlBtnW;
        const float dlY = modeY + modeH + 10.0f;
        const bool dlBusy = download_in_progress.load();
        if (dlBusy || !current_track_downloaded) {
            Gfx::rect(dlX, dlY, dlBtnW, dlBtnH, Gfx::COLOR_PANEL_HI);
            Gfx::rectOutline(dlX, dlY, dlBtnW, dlBtnH, 1, Gfx::COLOR_MUTED);
            Gfx::text(dlX + 10, dlY + 6, 0.75f, Gfx::COLOR_ACCENT,
                      dlBusy ? "Saving..." : "Download");
            if (!dlBusy) {
                add_ui_hit(dlX, dlY, dlBtnW, dlBtnH,
                           static_cast<int>(PlaybackUi::Download));
            }
        }
    }

    if (!status.empty()) {
        Gfx::textClipped(24, Gfx::SCREEN_H - 72, 0.78f, Gfx::COLOR_MUTED,
                         status.c_str(), Gfx::SCREEN_W - 48);
    }

    draw_footer("D-pad = move highlight   Cross = select   Touch = tap");
}

bool App::poll_quit_request() {
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 &&
        (pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) {
        return true;
    }
    if (!isRunning.load()) {
        return true;
    }
    return false;
}

void App::refresh_now_playing_metadata() {
    if (!cspot_started) {
        return;
    }

    cspot::TrackInfo info;
    {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (!g_spircHandler) {
            return;
        }
        if (!g_spircHandler->getTrackQueue()->peekHeadTrackInfo(info)) {
            return;
        }
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        if (!info.name.empty() && info.name != track_name) {
            track_name = info.name;
            changed = true;
        }
        if (!info.artist.empty() && info.artist != track_artist) {
            track_artist = info.artist;
            changed = true;
        }
        if (!info.album.empty() && info.album != track_album) {
            track_album = info.album;
            changed = true;
        }
        if (!info.imageUrl.empty() && info.imageUrl != track_image_url) {
            track_image_url = info.imageUrl;
            changed = true;
        }
    }
    if (info.duration > 0) {
        track_duration_ms.store(info.duration);
    }
    if (changed) {
        track_dirty = true;
    }
    refresh_current_track_downloaded();
}

void App::update_offline_album_art() {
    if (offline_art_path.empty() || !OfflineStore::coverFileExists(offline_art_path)) {
        if (!loaded_art_url.empty()) {
            Gfx::clearAlbumArt();
            loaded_art_url.clear();
        }
        return;
    }
    if (offline_art_path == loaded_art_url) {
        return;
    }
    loaded_art_url = offline_art_path;
    if (!Gfx::setAlbumArtFromFile(offline_art_path.c_str())) {
        Gfx::clearAlbumArt();
        loaded_art_url.clear();
    }
}

void App::update_album_art() {
    if (local_playback.load()) {
        update_offline_album_art();
        return;
    }

    std::string url;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        url = track_image_url;
    }
    if (url == loaded_art_url) {
        return;
    }
    loaded_art_url = url;

    if (url.empty()) {
        Gfx::clearAlbumArt();
        return;
    }

    uint8_t *buf = nullptr;
    int len = download(url.c_str(), &buf, "GET", "", {});
    if (len > 0 && buf != nullptr) {
        Gfx::setAlbumArtJpeg(buf, static_cast<unsigned long>(len));
    } else {
        Gfx::clearAlbumArt();
    }
    if (buf) {
        free(buf);
    }
}

void App::prepare_exit() {
    vitaspotify_release_all_audio_resources();
    isRunning = false;
    request_disconnect = true;
    stop_offline_playback();
    pl_dl_active = false;
    download_in_progress = false;
}

void App::poll_events() {
    if (vitaspotify_poll_livearea_quit(this)) {
        vitaspotify_terminate_process(this);
        return;
    }
    if (poll_quit_request()) {
        prepare_exit();
        return;
    }

    process_playback_commands();
    process_download_commands();
    process_playlist_download();

    // While a download is running it uses curl; don't fire a second concurrent
    // curl fetch for album art from the main thread.
    const bool dlBusy = download_in_progress.load() || pl_dl_active;

    if (local_playback.load()) {
        refresh_local_playback_state();
        if (!dlBusy) {
            update_offline_album_art();
        }
        request_redraw();
    } else if (cspot_started && !dlBusy) {
        refresh_now_playing_metadata();
        refresh_playback_transport_state();
        update_album_art();
    }

    if (auth_ok.exchange(false)) {
        screen = Screen::PLAYBACK;
        menu_index = 0;
        playback_focus = FocusPlayPause;
        set_status("In Spotify: pick PS Vita (VitaSpotify) as device.");
    }

    if (track_dirty.exchange(false)) {
        request_redraw();
    }

    static bool was_connecting = false;
    static bool was_auth_failed = false;
    const bool connecting = cspot_connecting.load();
    const bool failed = auth_failed.load();
    if (connecting != was_connecting || failed != was_auth_failed) {
        was_connecting = connecting;
        was_auth_failed = failed;
        request_redraw();
    }
}

bool App::handle_login_input() {
    SceCtrlData pad;
    static SceCtrlData old_pad = {};
    static bool pad_seeded = false;
    bool changed = false;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
        return false;
    }
    if (!pad_seeded) {
        old_pad = pad;
        pad_seeded = true;
        return false;
    }

    const int count = login_item_count(has_saved_session);
    const auto pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);

    if (pressed & SCE_CTRL_UP) {
        menu_index = (menu_index + count - 1) % count;
        changed = true;
    }
    if (pressed & SCE_CTRL_DOWN) {
        menu_index = (menu_index + 1) % count;
        changed = true;
    }

    if ((pressed & SCE_CTRL_CROSS) && !cspot_connecting &&
        !is_pad_confirm_blocked()) {
        if (menu_index == login_authjson_item_index(has_saved_session)) {
            login_with_authjson();
            changed = true;
        } else if (menu_index == login_offline_item_index(has_saved_session)) {
            enter_offline_library();
            changed = true;
        } else if (menu_index == login_quit_item_index(has_saved_session)) {
            prepare_exit();
        }
    }

    old_pad = pad;
    return changed;
}

void App::login_with_authjson() {
    std::string authData;
    if (g_file->readFile(CREDENTIALS_FILE_NAME, authData) && !authData.empty()) {
        try {
            g_loginBlob->loadJson(authData);
        } catch (const std::exception &e) {
            snprintf(auth_failure_msg, sizeof(auth_failure_msg),
                     "Bad auth.json: %s", e.what());
            auth_failed = true;
            cspot_connecting = false;
            request_redraw();
            return;
        }
        auth_failed = false;
        auth_failure_msg[0] = '\0';
        auth_ok = false;
        cspot_connecting = true;
        start_cspot_thread(this);
        request_redraw();
    } else {
        snprintf(auth_failure_msg, sizeof(auth_failure_msg),
                 "Missing ux0:data/vitaspotify/auth.json");
        auth_failed = true;
        request_redraw();
    }
}

void App::go_online() {
    request_disconnect = true;
    cspot_started = false;
    is_paused = true;
    if (local_playback.load()) {
        if (auto lp = vitaspotify_get_local_player()) {
            lp->setPaused(true);
        }
        stop_offline_playback();
        if (auto sink = vitaspotify_get_audio_sink()) {
            sink->clearBufferedAudio();
        }
    }
    vitaspotify_stop_network_thread();
    offline_library_mode = false;
    auth_failed = false;
    auth_failure_msg[0] = '\0';
    screen = Screen::LOGIN;
    request_redraw();
}

void App::login_activate_item(int item) {
    menu_index = item;
    if (cspot_connecting) {
        return;
    }
    if (item == login_authjson_item_index(has_saved_session)) {
        login_with_authjson();
    } else if (item == login_offline_item_index(has_saved_session)) {
        enter_offline_library();
        request_redraw();
    } else if (item == login_quit_item_index(has_saved_session)) {
        prepare_exit();
    }
}

bool App::handle_playback_input() {
    SceCtrlData pad;
    static SceCtrlData old_pad = {};
    static bool pad_seeded = false;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
        return false;
    }
    if (!pad_seeded) {
        old_pad = pad;
        pad_seeded = true;
        return false;
    }

    const auto pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);
    bool changed = false;

    if ((pressed & SCE_CTRL_CROSS) && !is_pad_confirm_blocked()) {
        activate_playback_focus();
        changed = true;
    } else if (pressed & SCE_CTRL_UP) {
        move_playback_focus(0, -1);
        changed = true;
    } else if (pressed & SCE_CTRL_DOWN) {
        move_playback_focus(0, 1);
        changed = true;
    } else if (pressed & SCE_CTRL_LEFT) {
        move_playback_focus(-1, 0);
        changed = true;
    } else if (pressed & SCE_CTRL_RIGHT) {
        move_playback_focus(1, 0);
        changed = true;
    }

    old_pad = pad;
    return changed;
}

// Converts a pasted Spotify share link or URI into a context URI cspot/spclient
// understands (e.g. "https://open.spotify.com/playlist/ID?si=x" ->
// "spotify:playlist:ID"). Returns empty if it doesn't look like Spotify content.
static std::string normalize_to_context_uri(std::string input) {
    while (!input.empty() &&
           (input.back() == ' ' || input.back() == '\r' || input.back() == '\n')) {
        input.pop_back();
    }
    size_t start = 0;
    while (start < input.size() && input[start] == ' ') {
        start++;
    }
    input = input.substr(start);

    if (input.rfind("spotify:", 0) == 0) {
        return input;
    }

    const std::string marker = "open.spotify.com/";
    const size_t pos = input.find(marker);
    if (pos == std::string::npos) {
        return "";
    }
    std::string path = input.substr(pos + marker.size());

    // Drop any query string / fragment.
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }
    // Strip a leading locale segment like "intl-de/".
    if (path.rfind("intl-", 0) == 0) {
        const size_t slash = path.find('/');
        if (slash != std::string::npos) {
            path = path.substr(slash + 1);
        }
    }

    const size_t slash = path.find('/');
    if (slash == std::string::npos) {
        return "";
    }
    std::string type = path.substr(0, slash);
    std::string id = path.substr(slash + 1);
    const size_t idEnd = id.find('/');
    if (idEnd != std::string::npos) {
        id = id.substr(0, idEnd);
    }
    if (type.empty() || id.empty()) {
        return "";
    }
    return "spotify:" + type + ":" + id;
}

void App::browse_resolve_and_play(const std::string &context_uri,
                                  const std::string &label) {
    if (!web_api_ready.load() || !api.has_spclient_base()) {
        set_status("Not ready yet — wait for connect, then retry.");
        return;
    }

    set_status("Resolving " + label + "...");
    render();

    std::vector<std::string> uris;
    int n = api.resolve_context_tracks(context_uri, uris);
    if (n <= 0 || uris.empty()) {
        set_status("Nothing playable found for " + label + ".");
        request_redraw();
        return;
    }

    if (playLocalCallback) {
        playLocalCallback(context_uri, uris, 0);
    }
    set_status("Playing " + label + " (" + std::to_string(uris.size()) +
               " tracks)");
    screen = Screen::PLAYBACK;
    menu_index = 0;
    request_redraw();
}

void App::open_results(const std::string &context_uri,
                       const std::string &title) {
    if (!web_api_ready.load() || !api.has_spclient_base()) {
        set_status("Not ready yet — wait for connect, then retry.");
        return;
    }

    results.clear();
    results_uris.clear();
    results_named.clear();
    results_index = 0;
    results_page = 0;
    results_context_uri = context_uri;
    results_title = title;
    screen = Screen::RESULTS;

    set_status("Loading...");
    render();

    // Pull a large window of track URIs; names are fetched lazily per page.
    api.resolve_context_tracks(context_uri, results_uris, 400);
    if (results_uris.empty()) {
        set_status("No results.");
        return;
    }

    results.resize(results_uris.size());
    results_named.assign(results_uris.size(), false);
    for (size_t i = 0; i < results_uris.size(); i++) {
        results[i].uri = results_uris[i];
    }

    ensure_page_loaded(0);
    refresh_results_download_state();
    set_status("");
}

void App::refresh_results_download_state() {
    results_dl_have = 0;
    results_dl_missing = 0;
    if (results_uris.empty()) {
        return;
    }
    const std::string name = results_title.empty() ? "Playlist" : results_title;
    std::vector<std::string> have;
    OfflineStore::downloadedUris(name, have);
    for (const auto &uri : results_uris) {
        if (std::find(have.begin(), have.end(), uri) != have.end()) {
            results_dl_have++;
        } else {
            results_dl_missing++;
        }
    }
}

void App::ensure_page_loaded(int page) {
    const int start = page * RESULTS_PAGE_SIZE;
    const int total = static_cast<int>(results_uris.size());
    if (start < 0 || start >= total) {
        return;
    }
    const int end = std::min(total, start + RESULTS_PAGE_SIZE);

    bool needFetch = false;
    for (int i = start; i < end; i++) {
        if (!results_named[i]) {
            needFetch = true;
            break;
        }
    }
    if (!needFetch) {
        return;
    }

    set_status("Loading names...");
    render();

    std::vector<std::string> pageUris(results_uris.begin() + start,
                                      results_uris.begin() + end);
    std::vector<TrackResult> named;
    api.fetch_track_results(pageUris, named, pageUris.size());

    for (size_t j = 0; j < named.size() && (start + (int)j) < total; j++) {
        results[start + j] = named[j];
        results_named[start + j] = true;
    }
    set_status("");
}

void App::draw_browse() {
    std::string status;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        status = status_line;
    }

    draw_screen_header(this, "Find Music", true);
    if (cspot_started) {
        draw_header_nav_button(this, "Go offline", false, UI_NAV_OFFLINE, 1);
    }

    const char *labels[BROWSE_MENU_COUNT] = {
        "Browse your Liked Songs",
        "Browse your playlists",
        "Play a Spotify link or URI",
        "Search songs / artists / albums",
        "Back"};
    const char *subs[BROWSE_MENU_COUNT] = {
        "Your saved tracks, page by page",
        "Your saved playlists, page by page",
        "Paste an open.spotify.com link",
        "Find anything and play it instantly",
        "Return to Now Playing"};

    float y = kContentTop;
    for (int i = 0; i < BROWSE_MENU_COUNT; i++) {
        const float rowTop = y;
        y = draw_row(y, i == browse_index, labels[i], subs[i]);
        add_ui_hit(24, rowTop, Gfx::SCREEN_W - 48, y - rowTop,
                   static_cast<int>(ListUi::RowBase) + i);
    }

    Gfx::text(24, y + 12, 0.8f, Gfx::COLOR_MUTED,
              "Plays directly on this Vita - no phone or PC needed.");

    if (!web_api_ready.load() || !api.has_spclient_base()) {
        Gfx::text(24, y + 40, 0.85f, Gfx::COLOR_ACCENT,
                  "Waiting for Spotify connection...");
    } else if (!status.empty()) {
        Gfx::textClipped(24, y + 40, 0.85f, Gfx::COLOR_SUBTEXT, status.c_str(),
                         Gfx::SCREEN_W - 48);
    }
    draw_footer("Touch or D-pad   Cross = select   Back or Circle = back");
}

bool App::handle_browse_input() {
    SceCtrlData pad;
    static SceCtrlData old_pad = {};
    static bool pad_seeded = false;
    bool changed = false;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
        return false;
    }
    if (!pad_seeded) {
        old_pad = pad;
        pad_seeded = true;
        return false;
    }

    const auto pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);

    if ((pressed & SCE_CTRL_CIRCLE) && !is_pad_confirm_blocked()) {
        screen = Screen::PLAYBACK;
        old_pad = pad;
        return true;
    }

    if (pressed & SCE_CTRL_UP) {
        browse_index = (browse_index + BROWSE_MENU_COUNT - 1) % BROWSE_MENU_COUNT;
        changed = true;
    }
    if (pressed & SCE_CTRL_DOWN) {
        browse_index = (browse_index + 1) % BROWSE_MENU_COUNT;
        changed = true;
    }

    if ((pressed & SCE_CTRL_CROSS) && !is_pad_confirm_blocked()) {
        if (browse_index == 0) {
            std::string uri = api.liked_songs_uri();
            if (uri.empty()) {
                set_status("Liked Songs unavailable (no user id yet).");
            } else {
                results_back = Screen::BROWSE;
                open_results(uri, "Liked Songs");
            }
            changed = true;
        } else if (browse_index == 1) {
            open_playlists();
            changed = true;
        } else if (browse_index == 2) {
            std::string link = Keyboard::GetText("Spotify link or URI", false);
            sceCtrlPeekBufferPositive(0, &old_pad, 1);
            if (!link.empty()) {
                std::string uri = normalize_to_context_uri(link);
                if (uri.empty()) {
                    set_status("That doesn't look like a Spotify link/URI.");
                } else {
                    browse_resolve_and_play(uri, uri);
                }
            }
            changed = true;
        } else if (browse_index == 3) {
            std::string query = Keyboard::GetText("Search Spotify", false);
            sceCtrlPeekBufferPositive(0, &old_pad, 1);
            if (!query.empty()) {
                results_back = Screen::BROWSE;
                open_results("spotify:search:" + query, "Results: " + query);
            }
            changed = true;
        } else {
            screen = Screen::PLAYBACK;
            changed = true;
        }
    }

    old_pad = pad;
    return changed;
}

void App::browse_activate_item(int item) {
    browse_index = item;
    if (item == 0) {
        std::string uri = api.liked_songs_uri();
        if (uri.empty()) {
            set_status("Liked Songs unavailable (no user id yet).");
        } else {
            results_back = Screen::BROWSE;
            open_results(uri, "Liked Songs");
        }
    } else if (item == 1) {
        open_playlists();
    } else if (item == 2) {
        std::string link = Keyboard::GetText("Spotify link or URI", false);
        request_redraw();
        if (!link.empty()) {
            std::string uri = normalize_to_context_uri(link);
            if (uri.empty()) {
                set_status("That doesn't look like a Spotify link/URI.");
            } else {
                browse_resolve_and_play(uri, uri);
            }
        }
    } else if (item == 3) {
        std::string query = Keyboard::GetText("Search Spotify", false);
        request_redraw();
        if (!query.empty()) {
            results_back = Screen::BROWSE;
            open_results("spotify:search:" + query, "Results: " + query);
        }
    } else {
        screen = Screen::PLAYBACK;
    }
}

void App::draw_results() {
    std::string status;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        status = status_line;
    }

    draw_screen_header(this, results_title.empty() ? "Results" : results_title.c_str(),
                       true);
    if (cspot_started && !results_uris.empty() && !pl_dl_active) {
        // Nothing saved yet -> "Download all". Some saved, some new -> "Resync".
        // Fully in sync -> no button at all.
        const char *dlLabel = nullptr;
        if (results_dl_have == 0) {
            dlLabel = "Download all";
        } else if (results_dl_missing > 0) {
            dlLabel = "Resync";
        }
        if (dlLabel) {
            draw_header_nav_button(this, dlLabel, false, UI_PL_DOWNLOAD, 1);
        }
    }

    const int total = static_cast<int>(results_uris.size());
    if (total == 0) {
        Gfx::text(24, kContentTop + 24, 1.0f, Gfx::COLOR_SUBTEXT,
                  status.empty() ? "No results." : status.c_str());
        draw_footer("Back (top-right) or Circle = back");
        return;
    }

    const int pageCount = (total + RESULTS_PAGE_SIZE - 1) / RESULTS_PAGE_SIZE;
    const int start = results_page * RESULTS_PAGE_SIZE;
    const int end = std::min(total, start + RESULTS_PAGE_SIZE);

    constexpr float kRowH = 40.0f;
    const float x = 24.0f;
    const float w = Gfx::SCREEN_W - 48.0f;
    float y = kContentTop;
    for (int i = start; i < end; i++) {
        const int local = i - start;
        const bool sel = (local == results_index);
        const float rowTop = y;
        if (sel) {
            Gfx::rect(x, y, w, kRowH, Gfx::COLOR_PANEL_HI);
            Gfx::rect(x, y, 4, kRowH, Gfx::COLOR_ACCENT);
        }
        Gfx::textf(x + 16, y + 9, 0.8f, Gfx::COLOR_MUTED, "%d", i + 1);

        const TrackResult &r = results[i];
        const uint32_t col = sel ? Gfx::COLOR_TEXT : Gfx::COLOR_SUBTEXT;
        if (!results_named[i]) {
            Gfx::text(x + 70, y + 9, 0.9f, Gfx::COLOR_MUTED, "Loading...");
        } else if (!r.artist.empty()) {
            char line[400];
            snprintf(line, sizeof(line), "%s   -   %s", r.title.c_str(),
                     r.artist.c_str());
            Gfx::textClipped(x + 70, y + 9, 0.9f, col, line, (int)w - 90);
        } else {
            Gfx::textClipped(x + 70, y + 9, 0.9f, col, r.title.c_str(),
                             (int)w - 90);
        }
        add_ui_hit(x, rowTop, w, kRowH + 2,
                   static_cast<int>(ListUi::RowBase) + local);
        y += kRowH + 2;
    }

    char hint[128];
    snprintf(hint, sizeof(hint),
             "Page %d/%d  -  %d songs    L/R = page    Cross = play    Circle = back",
             results_page + 1, pageCount, total);
    draw_footer(hint);
}

void App::open_playlists() {
    if (!web_api_ready.load() || !api.has_spclient_base()) {
        set_status("Not ready yet — wait for connect, then retry.");
        return;
    }

    pl_index = 0;
    pl_page = 0;
    screen = Screen::PLAYLISTS;

    if (!playlists_loaded) {
        set_status("Loading playlists...");
        render();
        playlists.clear();
        api.get_playlists(playlists, 200);
        playlists_loaded = true;
    }
    set_status(playlists.empty() ? "No playlists found." : "");
}

void App::draw_playlists() {
    std::string status;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        status = status_line;
    }

    draw_screen_header(this, "Your Playlists", true);

    const int total = static_cast<int>(playlists.size());
    if (total == 0) {
        Gfx::text(24, kContentTop + 24, 1.0f, Gfx::COLOR_SUBTEXT,
                  status.empty() ? "No playlists found." : status.c_str());
        draw_footer("Back (top-right) or Circle = back");
        return;
    }

    const int pageCount = (total + PLAYLISTS_PAGE_SIZE - 1) / PLAYLISTS_PAGE_SIZE;
    const int start = pl_page * PLAYLISTS_PAGE_SIZE;
    const int end = std::min(total, start + PLAYLISTS_PAGE_SIZE);

    constexpr float kRowH = 40.0f;
    const float x = 24.0f;
    const float w = Gfx::SCREEN_W - 48.0f;
    float y = kContentTop;
    for (int i = start; i < end; i++) {
        const int local = i - start;
        const bool sel = (local == pl_index);
        const float rowTop = y;
        if (sel) {
            Gfx::rect(x, y, w, kRowH, Gfx::COLOR_PANEL_HI);
            Gfx::rect(x, y, 4, kRowH, Gfx::COLOR_ACCENT);
        }
        Gfx::textf(x + 16, y + 9, 0.8f, Gfx::COLOR_MUTED, "%d", i + 1);
        const uint32_t col = sel ? Gfx::COLOR_TEXT : Gfx::COLOR_SUBTEXT;
        Gfx::textClipped(x + 70, y + 9, 0.9f, col, playlists[i].name.c_str(),
                         (int)w - 90);
        add_ui_hit(x, rowTop, w, kRowH + 2,
                   static_cast<int>(ListUi::RowBase) + local);
        y += kRowH + 2;
    }

    char hint[128];
    snprintf(hint, sizeof(hint),
             "Page %d/%d  -  %d playlists    L/R = page    Cross = open    Circle = back",
             pl_page + 1, pageCount, total);
    draw_footer(hint);
}

bool App::handle_playlists_input() {
    SceCtrlData pad;
    static SceCtrlData old_pad = {};
    static bool pad_seeded = false;
    bool changed = false;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
        return false;
    }
    if (!pad_seeded) {
        old_pad = pad;
        pad_seeded = true;
        return false;
    }

    const auto pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);
    const int total = static_cast<int>(playlists.size());

    if ((pressed & SCE_CTRL_CIRCLE) && !is_pad_confirm_blocked()) {
        screen = Screen::BROWSE;
        old_pad = pad;
        return true;
    }

    if (total > 0) {
        const int pageCount =
            (total + PLAYLISTS_PAGE_SIZE - 1) / PLAYLISTS_PAGE_SIZE;
        const int start = pl_page * PLAYLISTS_PAGE_SIZE;
        const int rowsOnPage = std::min(PLAYLISTS_PAGE_SIZE, total - start);

        if (pressed & SCE_CTRL_UP) {
            if (pl_index > 0) {
                pl_index--;
            } else if (pl_page > 0) {
                pl_page--;
                pl_index =
                    std::min(PLAYLISTS_PAGE_SIZE,
                             total - pl_page * PLAYLISTS_PAGE_SIZE) -
                    1;
            }
            changed = true;
        }
        if (pressed & SCE_CTRL_DOWN) {
            if (pl_index < rowsOnPage - 1) {
                pl_index++;
            } else if (pl_page < pageCount - 1) {
                pl_page++;
                pl_index = 0;
            }
            changed = true;
        }
        if ((pressed & SCE_CTRL_LTRIGGER) && pl_page > 0) {
            pl_page--;
            pl_index = 0;
            changed = true;
        }
        if ((pressed & SCE_CTRL_RTRIGGER) && pl_page < pageCount - 1) {
            pl_page++;
            pl_index = 0;
            changed = true;
        }
        if ((pressed & SCE_CTRL_CROSS) && !is_pad_confirm_blocked()) {
            const int absolute = start + pl_index;
            if (absolute >= 0 && absolute < total) {
                results_back = Screen::PLAYLISTS;
                open_results(playlists[absolute].uri, playlists[absolute].name);
            }
            changed = true;
        }
    }

    old_pad = pad;
    return changed;
}

bool App::handle_results_input() {
    SceCtrlData pad;
    static SceCtrlData old_pad = {};
    static bool pad_seeded = false;
    bool changed = false;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
        return false;
    }
    if (!pad_seeded) {
        old_pad = pad;
        pad_seeded = true;
        return false;
    }

    const auto pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);
    const int total = static_cast<int>(results_uris.size());

    if ((pressed & SCE_CTRL_CIRCLE) && !is_pad_confirm_blocked()) {
        screen = results_back;
        old_pad = pad;
        return true;
    }

    if (total > 0) {
        const int pageCount =
            (total + RESULTS_PAGE_SIZE - 1) / RESULTS_PAGE_SIZE;
        const int start = results_page * RESULTS_PAGE_SIZE;
        const int rowsOnPage = std::min(RESULTS_PAGE_SIZE, total - start);

        if (pressed & SCE_CTRL_UP) {
            if (results_index > 0) {
                results_index--;
            } else if (results_page > 0) {
                results_page--;
                ensure_page_loaded(results_page);
                results_index = std::min(RESULTS_PAGE_SIZE, total - results_page * RESULTS_PAGE_SIZE) - 1;
            }
            changed = true;
        }
        if (pressed & SCE_CTRL_DOWN) {
            if (results_index < rowsOnPage - 1) {
                results_index++;
            } else if (results_page < pageCount - 1) {
                results_page++;
                ensure_page_loaded(results_page);
                results_index = 0;
            }
            changed = true;
        }
        if ((pressed & SCE_CTRL_LTRIGGER) && results_page > 0) {
            results_page--;
            ensure_page_loaded(results_page);
            results_index = 0;
            changed = true;
        }
        if ((pressed & SCE_CTRL_RTRIGGER) && results_page < pageCount - 1) {
            results_page++;
            ensure_page_loaded(results_page);
            results_index = 0;
            changed = true;
        }
        if ((pressed & SCE_CTRL_CROSS) && !is_pad_confirm_blocked()) {
            const int absolute = start + results_index;
            if (absolute >= 0 && absolute < total && playLocalCallback) {
                playLocalCallback(results_context_uri, results_uris, absolute);
                const std::string label = results_named[absolute]
                                              ? results[absolute].title
                                              : std::string("track");
                set_status("Playing: " + label);
                screen = Screen::PLAYBACK;
                menu_index = 0;
            }
            changed = true;
        }
    }

    old_pad = pad;
    return changed;
}

void App::results_activate_row(int local_row) {
    const int total = static_cast<int>(results_uris.size());
    const int start = results_page * RESULTS_PAGE_SIZE;
    const int absolute = start + local_row;
    if (absolute < 0 || absolute >= total || !playLocalCallback) {
        return;
    }
    results_index = local_row;
    playLocalCallback(results_context_uri, results_uris, absolute);
    const std::string label =
        results_named[absolute] ? results[absolute].title : std::string("track");
    set_status("Playing: " + label);
    screen = Screen::PLAYBACK;
    menu_index = 0;
    request_redraw();
}

void App::playlists_activate_row(int local_row) {
    const int total = static_cast<int>(playlists.size());
    const int start = pl_page * PLAYLISTS_PAGE_SIZE;
    const int absolute = start + local_row;
    if (absolute < 0 || absolute >= total) {
        return;
    }
    pl_index = local_row;
    results_back = Screen::PLAYLISTS;
    open_results(playlists[absolute].uri, playlists[absolute].name);
}

void App::draw_offline_home() {
    draw_screen_header(this, "Offline Library", false);
    if (!cspot_started) {
        draw_header_nav_button(this, "Go online", false, UI_NAV_ONLINE, 0);
    }

    const int count = 1 + static_cast<int>(offline_playlists.size());
    float y = kContentTop;
    for (int i = 0; i < count; i++) {
        char buf[128];
        const char *label;
        const char *secondary = nullptr;
        if (i == 0) {
            label = "All Downloaded Songs";
            secondary = "Every track you've downloaded";
        } else {
            snprintf(buf, sizeof(buf), "%s", offline_playlists[i - 1].c_str());
            label = buf;
            secondary = "Downloaded playlist";
        }
        const float rowTop = y;
        y = draw_row(y, i == offline_home_index, label, secondary);
        add_ui_hit(24, rowTop, Gfx::SCREEN_W - 48, y - rowTop,
                   static_cast<int>(ListUi::RowBase) + i);
    }

    if (count == 1) {
        Gfx::text(24, y + 16, 0.85f, Gfx::COLOR_MUTED,
                  "Download songs or playlists while online to fill this up.");
    }
    draw_footer("Cross = open   Circle = sign in   Go online = sign in");
}

bool App::handle_offline_home_input() {
    SceCtrlData pad;
    static SceCtrlData old_pad = {};
    static bool pad_seeded = false;
    bool changed = false;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
        return false;
    }
    if (!pad_seeded) {
        old_pad = pad;
        pad_seeded = true;
        return false;
    }

    const auto pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);
    const int count = 1 + static_cast<int>(offline_playlists.size());

    if (pressed & SCE_CTRL_UP) {
        offline_home_index = (offline_home_index + count - 1) % count;
        changed = true;
    }
    if (pressed & SCE_CTRL_DOWN) {
        offline_home_index = (offline_home_index + 1) % count;
        changed = true;
    }
    if ((pressed & SCE_CTRL_CROSS) && !is_pad_confirm_blocked()) {
        open_offline_category(offline_home_index);
        changed = true;
    }
    if ((pressed & SCE_CTRL_CIRCLE) && !is_pad_confirm_blocked()) {
        navigate_back();
        changed = true;
    }

    old_pad = pad;
    return changed;
}

void App::draw_downloads() {
    std::string status;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        status = status_line;
    }

    const bool offline = !cspot_started;
    std::string title = current_category_all
                            ? "All Downloaded Songs"
                            : (current_category_playlist.empty()
                                   ? "Downloads"
                                   : current_category_playlist);
    draw_screen_header(this, title.c_str(), true);
    if (offline) {
        draw_header_nav_button(this, "Go online", false, UI_NAV_ONLINE, 1);
    }

    const int total = static_cast<int>(downloads.size());
    if (total == 0) {
        Gfx::text(24, kContentTop + 24, 1.0f, Gfx::COLOR_SUBTEXT,
                  status.empty() ? "No downloads yet. Use Download on Now Playing."
                                   : status.c_str());
        draw_footer(offline ? "Circle = back   Triangle / Go online = sign in"
                            : "Back or Circle = back");
        return;
    }

    const int pageCount =
        (total + DOWNLOADS_PAGE_SIZE - 1) / DOWNLOADS_PAGE_SIZE;
    const int start = dl_page * DOWNLOADS_PAGE_SIZE;
    const int end = std::min(total, start + DOWNLOADS_PAGE_SIZE);

    constexpr float kRowH = 40.0f;
    const float x = 24.0f;
    const float w = Gfx::SCREEN_W - 48.0f;
    float y = kContentTop;
    for (int i = start; i < end; i++) {
        const int local = i - start;
        const bool sel = (local == dl_index);
        const float rowTop = y;
        if (sel) {
            Gfx::rect(x, y, w, kRowH, Gfx::COLOR_PANEL_HI);
            Gfx::rect(x, y, 4, kRowH, Gfx::COLOR_ACCENT);
        }
        const OfflineTrackEntry &e = downloads[i];
        if (!e.artist.empty()) {
            char line[400];
            snprintf(line, sizeof(line), "%s   -   %s", e.title.c_str(),
                     e.artist.c_str());
            Gfx::textClipped(x + 16, y + 9, 0.9f,
                             sel ? Gfx::COLOR_TEXT : Gfx::COLOR_SUBTEXT, line,
                             (int)w - 32);
        } else {
            Gfx::textClipped(x + 16, y + 9, 0.9f,
                             sel ? Gfx::COLOR_TEXT : Gfx::COLOR_SUBTEXT,
                             e.title.c_str(), (int)w - 32);
        }
        add_ui_hit(x, rowTop, w, kRowH + 2,
                   static_cast<int>(ListUi::RowBase) + local);
        y += kRowH + 2;
    }

    char hint[160];
    if (offline) {
        snprintf(hint, sizeof(hint),
                 "Page %d/%d (%d tracks)   L/R = page   Cross = play   "
                 "Circle = back   Triangle = go online",
                 dl_page + 1, pageCount, total);
    } else {
        snprintf(hint, sizeof(hint),
                 "Page %d/%d (%d tracks)   L/R = page   Cross = play   "
                 "Circle = back",
                 dl_page + 1, pageCount, total);
    }
    draw_footer(hint);
}

bool App::handle_downloads_input() {
    SceCtrlData pad;
    static SceCtrlData old_pad = {};
    static bool pad_seeded = false;
    bool changed = false;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1) {
        return false;
    }
    if (!pad_seeded) {
        old_pad = pad;
        pad_seeded = true;
        return false;
    }

    const auto pressed = static_cast<unsigned int>(~old_pad.buttons & pad.buttons);
    const int total = static_cast<int>(downloads.size());

    if ((pressed & SCE_CTRL_TRIANGLE) && !cspot_started) {
        go_online();
        old_pad = pad;
        return true;
    }

    if ((pressed & SCE_CTRL_CIRCLE) && !is_pad_confirm_blocked()) {
        screen = downloads_return_screen;
        if (downloads_return_screen != Screen::LOGIN &&
            downloads_return_screen != Screen::OFFLINE_HOME) {
            offline_library_mode = false;
        }
        old_pad = pad;
        return true;
    }

    if (total > 0) {
        const int pageCount =
            (total + DOWNLOADS_PAGE_SIZE - 1) / DOWNLOADS_PAGE_SIZE;
        const int start = dl_page * DOWNLOADS_PAGE_SIZE;
        const int rowsOnPage = std::min(DOWNLOADS_PAGE_SIZE, total - start);

        if (pressed & SCE_CTRL_UP) {
            if (dl_index > 0) {
                dl_index--;
            } else if (dl_page > 0) {
                dl_page--;
                dl_index = std::min(DOWNLOADS_PAGE_SIZE, total - dl_page * DOWNLOADS_PAGE_SIZE) - 1;
            }
            changed = true;
        }
        if (pressed & SCE_CTRL_DOWN) {
            if (dl_index < rowsOnPage - 1) {
                dl_index++;
            } else if (dl_page < pageCount - 1) {
                dl_page++;
                dl_index = 0;
            }
            changed = true;
        }
        if ((pressed & SCE_CTRL_LTRIGGER) && dl_page > 0) {
            dl_page--;
            dl_index = 0;
            changed = true;
        }
        if ((pressed & SCE_CTRL_RTRIGGER) && dl_page < pageCount - 1) {
            dl_page++;
            dl_index = 0;
            changed = true;
        }
        if ((pressed & SCE_CTRL_CROSS) && !is_pad_confirm_blocked()) {
            const int absolute = start + dl_index;
            if (absolute >= 0 && absolute < total) {
                play_offline_file(downloads[absolute].path);
            }
            changed = true;
        }
    }

    old_pad = pad;
    return changed;
}

void App::downloads_activate_row(int local_row) {
    const int total = static_cast<int>(downloads.size());
    const int start = dl_page * DOWNLOADS_PAGE_SIZE;
    const int absolute = start + local_row;
    if (absolute < 0 || absolute >= total) {
        return;
    }
    dl_index = local_row;
    play_offline_file(downloads[absolute].path);
}

void App::render() {
    Gfx::beginFrame();
    clear_ui_hits();
    Gfx::clear(Gfx::COLOR_BG);
    if (screen == Screen::LOGIN) {
        draw_login();
    } else if (screen == Screen::BROWSE) {
        draw_browse();
    } else if (screen == Screen::PLAYLISTS) {
        draw_playlists();
    } else if (screen == Screen::DOWNLOADS) {
        draw_downloads();
    } else if (screen == Screen::OFFLINE_HOME) {
        draw_offline_home();
    } else if (screen == Screen::RESULTS) {
        draw_results();
    } else {
        draw_playback();
    }
    Gfx::endFrame();
}

void App::present_download_overlay(const std::string& label, int pct) {
    Gfx::beginFrame();
    Gfx::clear(Gfx::COLOR_BG);

    const float panelW = 560.0f;
    const float panelH = 180.0f;
    const float px = (Gfx::SCREEN_W - panelW) / 2.0f;
    const float py = (Gfx::SCREEN_H - panelH) / 2.0f;
    Gfx::rect(px, py, panelW, panelH, Gfx::COLOR_PANEL);
    Gfx::rectOutline(px, py, panelW, panelH, 2, Gfx::COLOR_PANEL_HI);

    Gfx::text(px + 28, py + 26, 1.1f, Gfx::COLOR_TEXT, "Downloading");
    Gfx::textClipped(px + 28, py + 64, 0.92f, Gfx::COLOR_SUBTEXT, label.c_str(),
                     static_cast<int>(panelW - 56));

    const float barX = px + 28;
    const float barY = py + 110;
    const float barW = panelW - 56;
    const float barH = 26.0f;
    Gfx::rect(barX, barY, barW, barH, Gfx::COLOR_PANEL_HI);

    if (pct >= 0) {
        const float frac = std::min(1.0f, std::max(0.0f, pct / 100.0f));
        Gfx::rect(barX, barY, barW * frac, barH, Gfx::COLOR_ACCENT);
        char pctText[16];
        snprintf(pctText, sizeof(pctText), "%d%%", pct);
        Gfx::text(barX, barY + barH + 10, 0.8f, Gfx::COLOR_MUTED, pctText);
    } else {
        // Indeterminate: a chunk sweeping left-to-right.
        const float chunkW = barW * 0.3f;
        const uint64_t t = sceKernelGetProcessTimeWide() / 4000;  // ~250/s
        const float travel = barW + chunkW;
        float pos = static_cast<float>(t % static_cast<uint64_t>(travel)) - chunkW;
        float drawX = std::max(barX, barX + pos);
        float drawW = std::min(chunkW, (barX + barW) - drawX);
        if (pos < 0) {
            drawW = chunkW + pos;
            drawX = barX;
        }
        if (drawW > 0) {
            Gfx::rect(drawX, barY, drawW, barH, Gfx::COLOR_ACCENT);
        }
        Gfx::text(barX, barY + barH + 10, 0.8f, Gfx::COLOR_MUTED,
                  "Working...");
    }

    Gfx::endFrame();
}

void App::run() {
    while (isRunning) {
        Touch::update();
        poll_events();
        if (!isRunning) {
            break;
        }

        const Screen screen_before_input = screen;

        render();
        process_touch();

        if (screen == Screen::LOGIN) {
            handle_login_input();
        } else if (screen == Screen::BROWSE) {
            handle_browse_input();
        } else if (screen == Screen::PLAYLISTS) {
            handle_playlists_input();
        } else if (screen == Screen::DOWNLOADS) {
            handle_downloads_input();
        } else if (screen == Screen::OFFLINE_HOME) {
            handle_offline_home_input();
        } else if (screen == Screen::RESULTS) {
            handle_results_input();
        } else {
            handle_playback_input();
        }
        needs_redraw = false;

        // A button held across a screen transition would otherwise look like a
        // fresh press to the next screen's handler (stale per-handler edge
        // state), causing a double activation. Briefly gate confirm/back right
        // after any screen change so the held button must be released first.
        if (screen != screen_before_input) {
            block_pad_confirm_after_touch(140);
        }

        sceKernelDelayThread(16000);
    }
}

void App::shutdown() {
    vitaspotify_shutdown_local_audio();
    Gfx::fini();
}
