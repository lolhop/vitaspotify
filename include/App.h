#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "API.h"
#include "OfflineStore.h"

struct BrowsePlaylist {
    std::string name;
    std::string uri;
};

// Touch hit region (screen coords); id is screen-specific action code.
struct UiHit {
    float x = 0, y = 0, w = 0, h = 0;
    int id = -1;
};

// Playback screen touch/control ids.
enum class PlaybackUi : int {
    Prev = 1,
    PlayPause = 2,
    Next = 3,
    Shuffle = 4,
    Repeat = 5,
    Browse = 6,
    SeekBar = 8,
    Download = 9,
};

// Queued from UI thread; executed in process_playback_commands on the main loop.
enum class TransportCmd : int {
    None = 0,
    TogglePlay = 1,
    Next = 2,
    Prev = 3,
    ToggleShuffle = 4,
    ToggleRepeat = 5,
};

enum class ListUi : int {
    RowBase = 100,
};

// Header back button (all screens except Now Playing).
static constexpr int UI_NAV_BACK = 300;
// Header "Go online" button (offline screens) -> returns to login.
static constexpr int UI_NAV_ONLINE = 301;
// Header "Offline mode" button (logged in) -> disconnect + offline home.
static constexpr int UI_NAV_OFFLINE = 302;
// Header "Download playlist" button (inside a playlist).
static constexpr int UI_PL_DOWNLOAD = 303;

class App {
 public:
    std::atomic<bool> isRunning{true};

    API api;
    bool cspot_started = false;
    std::atomic<bool> request_disconnect{false};
    std::atomic<bool> cspot_connecting{false};
    std::atomic<bool> auth_failed{false};
    std::atomic<bool> auth_ok{false};
    std::atomic<bool> web_api_ready{false};

    char username[256] = "";
    char password[256] = "";
    char auth_failure_msg[192] = "";

    std::mutex ui_mutex;
    std::string track_name = "No track";
    std::string track_artist = "";
    std::string track_album = "";
    std::string track_image_url = "";
    std::atomic<bool> is_paused{true};
    std::atomic<bool> track_dirty{false};
    std::atomic<uint32_t> track_duration_ms{0};
    std::atomic<uint32_t> track_position_ms{0};
    std::atomic<bool> shuffle_on{false};
    std::atomic<bool> repeat_on{false};

    // Starts local playback of a resolved track list on the Vita itself,
    // beginning at startIndex.
    std::function<void(const std::string &contextUri,
                       const std::vector<std::string> &trackUris, int startIndex)>
        playLocalCallback;

    void init();
    void run();
    void shutdown();
    // Stops playback, signals background threads to exit, and tears down audio.
    void prepare_exit();
    void request_redraw();
    void process_playback_commands();
    void process_download_commands();
    void on_download_finished(bool ok, const std::string& status);
    void request_download_current_track();
    void request_download_playlist();
    void process_playlist_download();
    void enter_offline_mode_from_online();
    // Renders a single progress frame; called from the curl transfer callback
    // so the screen animates instead of freezing during a blocking download.
    void present_download_overlay(const std::string& label, int pct);
    void play_offline_file(const std::string& oggPath);
    void stop_offline_playback();
    void enter_offline_library();
    void refresh_local_playback_state();
    void set_status(const std::string &msg);
    void refresh_now_playing_metadata();
    void refresh_playback_transport_state();

    void clear_ui_hits();
    void add_ui_hit(float x, float y, float w, float h, int id);

    std::atomic<bool> player_create_pending{false};
    std::atomic<int> pending_play_pause{-1};
    std::atomic<bool> pending_flush{false};
    std::atomic<bool> pending_playback_start{false};
    std::atomic<bool> pending_depleted{false};
    std::atomic<int> pending_volume{-1};
    std::atomic<int> pending_transport{0};
    std::atomic<bool> pending_seek{false};
    std::atomic<uint32_t> pending_seek_ms{0};

    std::atomic<bool> pending_download{false};
    std::atomic<bool> download_in_progress{false};
    std::atomic<bool> local_playback{false};
    bool offline_library_mode = false;
    bool local_shuffle_on = false;
    bool local_repeat_on = false;
    bool current_track_downloaded = false;
    int download_art_cooldown = 0;
    std::string offline_art_path;

    std::atomic<bool> download_done_pending{false};
    std::mutex download_done_mutex;
    bool download_done_ok = false;
    std::string download_done_msg;
    std::string download_done_title;

    // Playlist batch download: drives the cspot queue one track at a time,
    // saving each into the playlist's subfolder. Lives on the main loop.
    bool pl_dl_active = false;
    std::vector<std::string> pl_dl_uris;
    std::string pl_dl_context;
    std::string pl_dl_name;
    int pl_dl_index = 0;
    int pl_dl_ok = 0;
    int pl_dl_fail = 0;
    int pl_dl_phase = 0;  // 0 = (re)load track, 1 = wait for READY then save
    int pl_dl_wait_frames = 0;

 private:
    bool needs_redraw = true;
    enum class Screen {
        LOGIN,
        PLAYBACK,
        BROWSE,
        DOWNLOADS,
        PLAYLISTS,
        RESULTS,
        OFFLINE_HOME
    };

    Screen screen = Screen::LOGIN;
    int menu_index = 0;
    bool has_saved_session = false;
    std::string status_line;

    // Browse / play menu state
    int browse_index = 0;

    // Now Playing: which control has focus (see PlaybackFocus in App.cpp).
    int playback_focus = 3;

    // Results list (search / track lists), paged.
    std::vector<std::string> results_uris;  // every track uri in the context
    std::vector<TrackResult> results;        // names, filled lazily per page
    std::vector<bool> results_named;         // which entries have names yet
    std::string results_context_uri;
    std::string results_title;
    int results_index = 0;  // selection within the current page
    int results_page = 0;
    Screen results_back = Screen::BROWSE;  // where Circle returns from RESULTS
    // Download-sync state for the current track list (vs its saved subfolder).
    int results_dl_have = 0;     // tracks already downloaded
    int results_dl_missing = 0;  // tracks not yet downloaded
    void refresh_results_download_state();

    // User's saved playlists (paged), fetched in one rootlist call.
    std::vector<PlaylistEntry> playlists;
    bool playlists_loaded = false;
    int pl_index = 0;  // selection within the current page
    int pl_page = 0;

    std::vector<OfflineTrackEntry> downloads;
    int dl_index = 0;   // selection within the current page
    int dl_page = 0;
    int offline_play_index = -1;
    Screen downloads_return_screen = Screen::BROWSE;

    // Offline library home (category list): "All Downloaded Songs" + each
    // downloaded playlist subfolder.
    std::vector<std::string> offline_playlists;
    int offline_home_index = 0;
    std::string current_category_playlist;  // "" with all=false -> loose root
    bool current_category_all = false;
    void refresh_downloads_list();
    void draw_offline_home();
    bool handle_offline_home_input();
    void open_offline_category(int index);
    void offline_home_activate_row(int row);

    // Downloads + uploads the currently playing track's cover art to a GPU
    // texture when the art URL changes. Runs on the render thread.
    void update_album_art();
    void update_offline_album_art();
    std::string loaded_art_url;  // render-thread only

    std::vector<UiHit> ui_hits;
    float seek_bar_x = 0, seek_bar_y = 0, seek_bar_w = 0, seek_bar_h = 0;

    // Front-panel touch can make Cross look pressed on the next frame; ignore
    // confirm briefly after handling a touch hit.
    uint64_t pad_confirm_blocked_until_us = 0;
    void block_pad_confirm_after_touch(unsigned ms = 400);
    bool is_pad_confirm_blocked() const;

    void process_touch();
    int hit_test(float x, float y) const;

    void render();
    void poll_events();
    bool poll_quit_request();
    bool handle_login_input();
    bool handle_playback_input();
    bool handle_browse_input();
    bool handle_results_input();
    bool handle_playlists_input();
    bool handle_downloads_input();
    void draw_login();
    void draw_playback();
    void draw_browse();
    void draw_results();
    void draw_playlists();
    void draw_downloads();
    void open_playlists();
    void browse_resolve_and_play(const std::string &context_uri,
                                 const std::string &label);
    void open_results(const std::string &context_uri, const std::string &title);
    void ensure_page_loaded(int page);

    void playback_action_prev();
    void playback_action_next();
    void playback_action_toggle();
    void playback_action_shuffle();
    void playback_action_repeat();
    void queue_transport(TransportCmd cmd);
    void playback_action_browse();
    void playback_action_seek_at(float x);
    void move_playback_focus(int dx, int dy);
    void activate_playback_focus();
    void set_playback_focus_from_ui(int uiId);
    void navigate_back();

    void login_activate_item(int item);
    void login_with_authjson();
    void go_online();
    void browse_activate_item(int item);
    void results_activate_row(int local_row);
    void playlists_activate_row(int local_row);
    void downloads_activate_row(int local_row);
    void open_downloads();
    void open_downloads(Screen returnTo);
    void play_offline_relative(int delta);
    void handle_local_track_ended();
    void refresh_current_track_downloaded();
};

void start_cspot_thread(App *app);
