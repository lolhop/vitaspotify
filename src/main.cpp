#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>


#include <LoginBlob.h>
#include <Logger.h>
#include <MercurySession.h>
#include <SpircHandler.h>
#include <TrackPlayer.h>
#include <CSpotContext.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "App.h"
#include "CliFile.h"
#include "Config.h"
#include "Log.h"
#include "Utils.h"
#include "LocalPlayer.h"
#include "OfflineStore.h"
#include "VitaAudioSink.h"
#include "VitaPlayer.h"

#include <TrackQueue.h>
#include <CDNAudioFile.h>

#include <AccessKeyFetcher.h>
#include <ApResolve.h>

#include "cJSON.h"

std::shared_ptr<CliFile> g_file;
std::shared_ptr<cspot::LoginBlob> g_loginBlob;
std::shared_ptr<cspot::Context> g_ctx;
std::shared_ptr<cspot::SpircHandler> g_spircHandler;
std::shared_ptr<VitaPlayer> g_player;

std::atomic<bool> g_playbackPaused{true};

static int watch_id = -1;
static int quit_listener_id = -1;
static int cspot_id = -1;
std::mutex g_cspot_mutex;

static void init_app_util() {
    sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
    SceAppUtilInitParam initParam{};
    SceAppUtilBootParam bootParam{};
    const int ret = sceAppUtilInit(&initParam, &bootParam);
    if (ret < 0 && ret != static_cast<int>(SCE_APPUTIL_ERROR_BUSY)) {
        CSPOT_LOG(info, "sceAppUtilInit -> 0x%08X", ret);
    }
}

namespace {

bool app_mgr_event_is_quit(const SceAppMgrEvent &ev) {
    return ev.event == SCE_APP_EVENT_REQUEST_QUIT;
}

bool drain_app_mgr_events() {
    bool quit = false;
    for (int i = 0; i < 8; ++i) {
        SceAppMgrEvent ev{};
        unsigned int timeout = 0;
        const int num = sceAppMgrReceiveEventNum(1, &ev, &timeout);
        if (num > 0 && ev.event != 0) {
            CSPOT_LOG(info, "AppMgr event (num=%d): 0x%08X", num, ev.event);
            quit = quit || app_mgr_event_is_quit(ev);
            continue;
        }

        memset(&ev, 0, sizeof(ev));
        if (sceAppMgrReceiveEvent(&ev) >= 0 && ev.event != 0) {
            CSPOT_LOG(info, "AppMgr event (recv): 0x%08X", ev.event);
            quit = quit || app_mgr_event_is_quit(ev);
            continue;
        }
        break;
    }
    return quit;
}

bool poll_app_util_event() {
    SceAppUtilAppEventParam param{};
    const int ret = sceAppUtilReceiveAppEvent(&param);
    if (ret < 0) {
        return false;
    }
    CSPOT_LOG(info, "AppUtil event: 0x%08X", param.type);
    return false;
}

bool poll_pending_app_mgr_events() {
    SceAppMgrAppState state{};
    if (sceAppMgrGetAppInfo(nullptr, &state) < 0) {
        return false;
    }
    if (state.appEventNum == 0) {
        return false;
    }
    return drain_app_mgr_events();
}

}  // namespace

bool vitaspotify_poll_livearea_quit(App * /*app*/) {
    if (drain_app_mgr_events()) {
        return true;
    }
    if (poll_app_util_event()) {
        return true;
    }
    return poll_pending_app_mgr_events();
}

namespace {

std::atomic<bool> g_audio_resources_released{false};

}  // namespace

void vitaspotify_release_all_audio_resources() {
    if (g_audio_resources_released.exchange(true)) {
        return;
    }
    CSPOT_LOG(info, "Releasing BGM and audio ports");
    vitaspotify_force_release_audio();
    sceAppMgrReleaseBgmPort();
}

void vitaspotify_cancel_network_thread() {
    // Vita has no userland thread-cancel API; ExitProcess follows immediately.
    (void)cspot_id;
}

// LiveArea swipe-close must hard-exit without blocking on mutexes or C++
// destructors — g_player keeps the audio sink alive if we only reset globals.
void vitaspotify_terminate_process(App *app) {
    CSPOT_LOG(info, "Hard terminate (LiveArea quit)");
    if (app) {
        app->request_disconnect = true;
        app->isRunning = false;
    }
    vitaspotify_emergency_stop_playback();
    VitaAudioSink::emergencyKillAll();
    vitaspotify_cancel_network_thread();
    sceAppMgrQuitForNonSuspendableApp();
    flush_logger();
    sceKernelExitProcess(0);
}

SceVoid quit_listener(SceSize /*args*/, void *argp) {
    auto *app = *static_cast<App **>(argp);
    while (app->isRunning) {
        if (vitaspotify_poll_livearea_quit(app)) {
            vitaspotify_terminate_process(app);
        }
        sceKernelDelayThread(50000);
    }
}

SceVoid watch_dog(SceSize /*args*/, void *argp) {
    auto *app = *static_cast<App **>(argp);

    while (app->isRunning) {
        sceKernelDelayThread(1000000);
        const bool onlinePlaying = app->cspot_started && !g_playbackPaused.load();
        const bool offlinePlaying = app->local_playback.load();
        if (onlinePlaying || offlinePlaying) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        }
    }
}

static void trim_credential(char *value) {
    if (value == nullptr || value[0] == '\0') {
        return;
    }
    char *end = value + strlen(value) - 1;
    while (end >= value && std::isspace(static_cast<unsigned char>(*end))) {
        *end-- = '\0';
    }
    char *start = value;
    while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start))) {
        start++;
    }
    if (start != value) {
        memmove(value, start, strlen(start) + 1);
    }
}

void login_cspot(const char *user, const char *password) {
    char user_buf[256];
    char pass_buf[256];
    strncpy(user_buf, user, sizeof(user_buf) - 1);
    user_buf[sizeof(user_buf) - 1] = '\0';
    strncpy(pass_buf, password, sizeof(pass_buf) - 1);
    pass_buf[sizeof(pass_buf) - 1] = '\0';
    trim_credential(user_buf);
    trim_credential(pass_buf);
    g_loginBlob->loadUserPass(user_buf, pass_buf);
}

static std::shared_ptr<cspot::AccessKeyFetcher> g_webApiTokenFetcher;

static void fetch_web_api_token(App *app) {
    if (!g_ctx || !g_ctx->session) {
        return;
    }

    app->api.set_device_id(g_ctx->config.deviceId);

    // login5 stored-credential tokens for the Spotify client carry full scope,
    // so we can drive the Web API (playlists, transfer/play) with the same path
    // cspot already uses internally for playback. This call blocks briefly.
    try {
        g_webApiTokenFetcher = std::make_shared<cspot::AccessKeyFetcher>(g_ctx);
        std::string token = g_webApiTokenFetcher->getAccessKey();
        if (!token.empty()) {
            app->api.set_token(token);
            // Resolve the spclient backend host (not rate-limited like the
            // public Web API) for context/track resolution.
            try {
                std::string hostPort = cspot::ApResolve("").fetchFirstSpclientAddress();
                std::string base;
                const auto colon = hostPort.find(':');
                if (colon == std::string::npos) {
                    base = "https://" + hostPort;
                } else {
                    base = "https://" + hostPort.substr(0, colon) + ":" +
                           hostPort.substr(colon + 1);
                }
                app->api.set_spclient_base(base);
                CSPOT_LOG(info, "spclient base: %s", base.c_str());
                app->api.set_username(g_ctx->config.username);
            } catch (...) {
                CSPOT_LOG(error, "spclient base resolve failed");
            }
            app->web_api_ready = true;
            CSPOT_LOG(info, "Web API token ready (len %d)", (int)token.size());
        } else {
            CSPOT_LOG(error, "Web API token fetch returned empty");
        }
    } catch (const std::exception &e) {
        CSPOT_LOG(error, "Web API token fetch failed: %s", e.what());
    } catch (...) {
        CSPOT_LOG(error, "Web API token fetch failed");
    }
}

static void stop_track_pipeline_locked() {
    if (g_spircHandler) {
        if (auto tp = g_spircHandler->getTrackPlayer()) {
            tp->setDataCallback(
                [](uint8_t *, size_t, std::string_view) { return 0; });
            tp->stop();
        }
        g_spircHandler->getTrackQueue()->stopTask();
    }
}

static void teardown_cspot_globals() {
    std::lock_guard<std::mutex> lock(g_cspot_mutex);
    stop_track_pipeline_locked();
    if (g_player) {
        g_player->disconnect();
        g_player.reset();
    }
    if (g_ctx && g_ctx->session) {
        g_ctx->session->disconnect();
    }
    g_spircHandler.reset();
    g_webApiTokenFetcher.reset();
    g_ctx.reset();
}

void vitaspotify_halt_online_playback() {
    std::lock_guard<std::mutex> lock(g_cspot_mutex);
    // Mirror user pause: silence speakers before stopping decode threads.
    if (g_player) {
        g_player->onPlayPause(true);
        g_player->onFlush();
    }
    g_playbackPaused = true;

    stop_track_pipeline_locked();

    if (g_player) {
        g_player->stopPlayback();
    }
    if (auto sink = vitaspotify_get_audio_sink()) {
        sink->clearBufferedAudio();
    }
}

void vitaspotify_stop_spotify_pipeline() {
    vitaspotify_halt_online_playback();
}

static void stop_cspot_thread() {
    if (cspot_id < 0) {
        return;
    }
    sceKernelWaitThreadEnd(cspot_id, nullptr, nullptr);
    sceKernelDeleteThread(cspot_id);
    cspot_id = -1;
    teardown_cspot_globals();
}

void vitaspotify_stop_network_thread() {
    stop_cspot_thread();
}

void App::process_playback_commands() {
    if (player_create_pending.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (!g_player && g_spircHandler) {
            try {
                if (g_player) {
                    g_player->disconnect();
                    g_player.reset();
                }
                vitaspotify_ensure_local_audio();
                g_player = std::make_shared<VitaPlayer>(
                    vitaspotify_get_audio_sink(), g_spircHandler);
                g_player->onPlayPause(g_playbackPaused.load());
                if (g_ctx && g_ctx->config.volume > 0) {
                    g_player->onVolume(static_cast<int>(g_ctx->config.volume));
                } else {
                    g_player->onVolume(65535 / 2);
                }
                refresh_now_playing_metadata();
                CSPOT_LOG(info, "Audio player ready");
            } catch (const std::exception &e) {
                CSPOT_LOG(error, "Player init failed: %s", e.what());
            } catch (...) {
                CSPOT_LOG(error, "Player init failed");
            }
        }
    }

    if (pending_flush.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_player) {
            g_player->onFlush();
        }
    }

    if (pending_playback_start.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_player) {
            g_player->onPlaybackStart();
        }
    }

    const int play_pause = pending_play_pause.exchange(-1);
    if (play_pause >= 0) {
        g_playbackPaused = play_pause != 0;
        is_paused = g_playbackPaused.load();
        track_dirty = true;
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_player) {
            g_player->onPlayPause(g_playbackPaused);
        }
    }

    const int volume = pending_volume.exchange(-1);
    if (volume >= 0) {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_player) {
            g_player->onVolume(volume);
        }
    }

    if (pending_depleted.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_player) {
            g_player->onDepleted();
        }
    }

    const int transport = pending_transport.exchange(
        static_cast<int>(TransportCmd::None));
    if (transport != static_cast<int>(TransportCmd::None)) {
        if (local_playback.load()) {
            switch (static_cast<TransportCmd>(transport)) {
                case TransportCmd::TogglePlay:
                    playback_action_toggle();
                    break;
                case TransportCmd::Prev:
                    play_offline_relative(-1);
                    break;
                case TransportCmd::Next:
                    play_offline_relative(1);
                    break;
                case TransportCmd::ToggleShuffle:
                    playback_action_shuffle();
                    break;
                case TransportCmd::ToggleRepeat:
                    playback_action_repeat();
                    break;
                default:
                    break;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_spircHandler) {
            switch (static_cast<TransportCmd>(transport)) {
                case TransportCmd::TogglePlay:
                    g_spircHandler->setPause(!g_playbackPaused.load());
                    break;
                case TransportCmd::Next:
                    if (g_spircHandler->nextSong()) {
                        g_spircHandler->setPause(false);
                        g_playbackPaused = false;
                        pending_play_pause = 0;
                        is_paused = false;
                        track_dirty = true;
                        if (g_player) {
                            g_player->onPlayPause(false);
                        }
                    }
                    break;
                case TransportCmd::Prev:
                    if (g_spircHandler->previousSong()) {
                        g_spircHandler->setPause(false);
                        g_playbackPaused = false;
                        pending_play_pause = 0;
                        is_paused = false;
                        track_dirty = true;
                        if (g_player) {
                            g_player->onPlayPause(false);
                        }
                    } else {
                        g_spircHandler->getTrackPlayer()->seekMs(0);
                        g_spircHandler->setPause(false);
                    }
                    break;
                case TransportCmd::ToggleShuffle:
                    g_spircHandler->toggleShuffle();
                    shuffle_on = g_spircHandler->isShuffleOn();
                    track_dirty = true;
                    break;
                case TransportCmd::ToggleRepeat:
                    g_spircHandler->toggleRepeat();
                    repeat_on = g_spircHandler->isRepeatOn();
                    track_dirty = true;
                    break;
                default:
                    break;
            }
        }
    }

    if (pending_seek.exchange(false)) {
        const uint32_t seekMs = pending_seek_ms.load();
        if (local_playback.load()) {
            if (auto lp = vitaspotify_get_local_player()) {
                lp->seekMs(seekMs);
                track_position_ms = seekMs;
                track_dirty = true;
            }
        } else {
            std::lock_guard<std::mutex> lock(g_cspot_mutex);
            if (g_spircHandler) {
                g_spircHandler->seekToMs(seekMs);
                track_position_ms = seekMs;
                track_dirty = true;
                if (g_player) {
                    g_player->onFlush();
                }
            }
        }
    }
}

namespace {

// The download runs on the MAIN thread using curl (the exact same curl path
// album art uses, which is rock-solid on this device) plus AES-CTR decryption.
// We never open a second cspot/bell stream, so it cannot race the live session.
// It is two-phase so the "Saving..." status renders before the blocking fetch.
struct PendingDownload {
    bool armed = false;
    std::string cdnUrl;
    std::vector<uint8_t> audioKey;
    std::string title;
    std::string artist;
    std::string imageUrl;
    std::string uri;
};

PendingDownload g_pendingDl;

}  // namespace

void App::process_download_commands() {
    // Phase 2: actually perform the (blocking) download on the main thread.
    if (g_pendingDl.armed) {
        g_pendingDl.armed = false;
        std::string path;
        std::string err;
        CSPOT_LOG(info, "process_download_commands: begin save \"%s\"",
                  g_pendingDl.title.c_str());
        const std::string overlayLabel = g_pendingDl.title;
        auto progress = [this, overlayLabel](size_t dlnow, size_t dltotal) {
            const int pct =
                dltotal > 0 ? static_cast<int>((dlnow * 100) / dltotal) : -1;
            present_download_overlay(overlayLabel, pct);
        };
        const bool ok = OfflineStore::saveCdnTrack(
            g_pendingDl.cdnUrl, g_pendingDl.audioKey, g_pendingDl.title,
            g_pendingDl.artist, /*playlist=*/"", g_pendingDl.uri, path, err,
            progress);
        if (ok && !g_pendingDl.imageUrl.empty()) {
            OfflineStore::saveCoverFromUrl(g_pendingDl.title,
                                           g_pendingDl.imageUrl, /*playlist=*/"");
        }
        download_in_progress = false;
        std::string msg;
        if (ok) {
            msg = "Downloaded: " + g_pendingDl.title;
        } else {
            msg = "Download failed";
            if (!err.empty()) {
                msg += " (" + err + ")";
            }
        }
        CSPOT_LOG(info, "process_download_commands: done ok=%d", ok ? 1 : 0);
        g_pendingDl = PendingDownload{};
        on_download_finished(ok, msg);
        return;
    }

    if (!pending_download.exchange(false)) {
        return;
    }
    if (download_in_progress.load()) {
        return;
    }

    std::shared_ptr<cspot::QueuedTrack> track;
    std::string cdnUrl;
    std::vector<uint8_t> audioKey;
    std::string title;
    std::string artist;
    std::string imageUrl;
    std::string uri;

    {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (!g_spircHandler) {
            set_status("Not connected.");
            return;
        }
        auto queue = g_spircHandler->getTrackQueue();
        if (!queue || !queue->getCurrentPlayingTrack(track) ||
            track->state != cspot::QueuedTrack::State::READY) {
            set_status("No track ready to download.");
            return;
        }
        cdnUrl = track->getCdnUrl();
        audioKey = track->getAudioKey();
        title = track->trackInfo.name.empty() ? "track" : track->trackInfo.name;
        artist = track->trackInfo.artist;
        imageUrl = track->trackInfo.imageUrl;
        uri = track->ref.uri;
    }

    if (cdnUrl.empty() || audioKey.empty()) {
        set_status("Track stream not available.");
        return;
    }

    if (OfflineStore::hasDownloadAnywhere(title)) {
        current_track_downloaded = true;
        set_status("Already downloaded.");
        request_redraw();
        return;
    }

    // Pause audio output while saving (purely local; cspot keeps its stream).
    {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (g_spircHandler) {
            g_spircHandler->setPause(true);
        }
        if (g_player) {
            g_player->onPlayPause(true);
        }
        g_playbackPaused = true;
        is_paused = true;
    }

    g_pendingDl.armed = true;
    g_pendingDl.cdnUrl = cdnUrl;
    g_pendingDl.audioKey = audioKey;
    g_pendingDl.title = title;
    g_pendingDl.artist = artist;
    g_pendingDl.imageUrl = imageUrl;
    g_pendingDl.uri = uri;
    download_in_progress = true;
    set_status("Saving \"" + title + "\"...");
    request_redraw();
}

void App::request_download_playlist() {
    if (pl_dl_active) {
        return;
    }
    if (!cspot_started) {
        set_status("Connect to Spotify to download playlists.");
        return;
    }
    if (results_uris.empty()) {
        set_status("Nothing to download.");
        return;
    }
    pl_dl_context = results_context_uri;
    pl_dl_name = results_title.empty() ? "Playlist" : results_title;

    // Resync: only fetch tracks that aren't already saved in this folder.
    std::vector<std::string> have;
    OfflineStore::downloadedUris(pl_dl_name, have);
    pl_dl_uris.clear();
    for (const auto& uri : results_uris) {
        if (std::find(have.begin(), have.end(), uri) == have.end()) {
            pl_dl_uris.push_back(uri);
        }
    }
    if (pl_dl_uris.empty()) {
        set_status("Playlist already fully downloaded.");
        refresh_results_download_state();
        request_redraw();
        return;
    }
    pl_dl_index = 0;
    pl_dl_ok = 0;
    pl_dl_fail = 0;
    pl_dl_phase = 0;
    pl_dl_wait_frames = 0;
    pl_dl_active = true;
    set_status("Downloading playlist: " + pl_dl_name);
    request_redraw();
}

// Drives the cspot queue one track at a time, saving each into the playlist's
// subfolder. Runs on the main loop so the UI keeps rendering progress between
// tracks (only the per-track curl fetch blocks, just like single downloads).
void App::process_playlist_download() {
    if (!pl_dl_active) {
        return;
    }

    const int total = static_cast<int>(pl_dl_uris.size());
    if (pl_dl_index >= total) {
        pl_dl_active = false;
        char msg[160];
        snprintf(msg, sizeof(msg), "Playlist saved: %s (%d ok, %d failed)",
                 pl_dl_name.c_str(), pl_dl_ok, pl_dl_fail);
        set_status(msg);
        refresh_results_download_state();
        request_redraw();
        return;
    }

    // Phase 0: load the next track into the queue (no audio playback).
    if (pl_dl_phase == 0) {
        const std::string uri = pl_dl_uris[static_cast<size_t>(pl_dl_index)];
        {
            std::lock_guard<std::mutex> lock(g_cspot_mutex);
            if (!g_spircHandler) {
                pl_dl_active = false;
                set_status("Disconnected; playlist download stopped.");
                request_redraw();
                return;
            }
            g_spircHandler->loadAndPlayTracks({uri}, pl_dl_context, 0);
            g_spircHandler->setPause(true);
            if (g_player) {
                g_player->onPlayPause(true);
                g_player->onFlush();
            }
            g_playbackPaused = true;
            is_paused = true;
        }
        pl_dl_phase = 1;
        pl_dl_wait_frames = 0;
        char st[160];
        snprintf(st, sizeof(st), "Downloading %s: %d/%d", pl_dl_name.c_str(),
                 pl_dl_index + 1, total);
        set_status(st);
        request_redraw();
        return;
    }

    // Phase 1: wait for the track to become READY, then save it (blocking).
    std::shared_ptr<cspot::QueuedTrack> track;
    std::string cdnUrl;
    std::string title;
    std::string artist;
    std::string imageUrl;
    std::vector<uint8_t> audioKey;
    bool ready = false;
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (!g_spircHandler) {
            pl_dl_active = false;
            set_status("Disconnected; playlist download stopped.");
            request_redraw();
            return;
        }
        auto queue = g_spircHandler->getTrackQueue();
        if (queue && queue->getCurrentPlayingTrack(track) && track) {
            if (track->state == cspot::QueuedTrack::State::READY) {
                cdnUrl = track->getCdnUrl();
                audioKey = track->getAudioKey();
                title = track->trackInfo.name.empty() ? "track"
                                                      : track->trackInfo.name;
                artist = track->trackInfo.artist;
                imageUrl = track->trackInfo.imageUrl;
                ready = !cdnUrl.empty() && !audioKey.empty();
            } else if (track->state == cspot::QueuedTrack::State::FAILED) {
                failed = true;
            }
        }
    }

    if (ready) {
        const std::string trackUri = pl_dl_uris[static_cast<size_t>(pl_dl_index)];
        char overlayBuf[160];
        snprintf(overlayBuf, sizeof(overlayBuf), "%s  (%d/%d)  %s",
                 pl_dl_name.c_str(), pl_dl_index + 1, total, title.c_str());
        const std::string overlayLabel = overlayBuf;
        auto progress = [this, overlayLabel](size_t dlnow, size_t dltotal) {
            const int pct =
                dltotal > 0 ? static_cast<int>((dlnow * 100) / dltotal) : -1;
            present_download_overlay(overlayLabel, pct);
        };
        std::string path;
        std::string err;
        if (OfflineStore::saveCdnTrack(cdnUrl, audioKey, title, artist,
                                       pl_dl_name, trackUri, path, err,
                                       progress)) {
            if (!imageUrl.empty()) {
                OfflineStore::saveCoverFromUrl(title, imageUrl, pl_dl_name);
            }
            pl_dl_ok++;
        } else {
            pl_dl_fail++;
        }
        pl_dl_index++;
        pl_dl_phase = 0;
        request_redraw();
        return;
    }

    // ~10s per track to fetch metadata + key + CDN url before giving up.
    if (failed || ++pl_dl_wait_frames > 600) {
        pl_dl_fail++;
        pl_dl_index++;
        pl_dl_phase = 0;
        return;
    }
}

int start_cspot(SceSize /*args*/, void *argp) {
    auto *app = *static_cast<App **>(argp);

    app->auth_failure_msg[0] = '\0';
    CSPOT_LOG(info, "Connecting to Spotify...");

    try {
        g_ctx = cspot::Context::createFromBlob(g_loginBlob);
        g_ctx->config.audioFormat = AudioFormat_OGG_VORBIS_96;
        g_ctx->config.volume = 65535 / 2;

        g_ctx->session->connectWithRandomAp();
        g_ctx->config.authData = g_ctx->session->authenticate(g_loginBlob);

        if (g_ctx->config.authData.empty()) {
            CSPOT_LOG(error, "Authentication failed");
            snprintf(app->auth_failure_msg, sizeof(app->auth_failure_msg),
                     "Session rejected. Re-copy auth.json from Mac.");
            app->cspot_connecting = false;
            app->auth_failed = true;
            g_ctx.reset();
            return 0;
        }

        app->cspot_connecting = false;
        app->auth_ok = true;

        g_file->writeFile(CREDENTIALS_FILE_NAME, g_ctx->getCredentialsJson());

        {
            std::lock_guard<std::mutex> lock(g_cspot_mutex);
            g_spircHandler = std::make_shared<cspot::SpircHandler>(g_ctx);
        }
        if (app->request_disconnect.load()) {
            CSPOT_LOG(info, "Connect aborted (mode switch)");
            teardown_cspot_globals();
            app->cspot_connecting = false;
            app->cspot_started = false;
            return 0;
        }

        app->player_create_pending = true;
        app->cspot_started = true;

        fetch_web_api_token(app);

        if (app->request_disconnect.load()) {
            CSPOT_LOG(info, "Connect aborted after token fetch (mode switch)");
            teardown_cspot_globals();
            app->cspot_connecting = false;
            app->cspot_started = false;
            return 0;
        }
    } catch (const std::exception &e) {
        CSPOT_LOG(error, "start_cspot failed: %s", e.what());
        snprintf(app->auth_failure_msg, sizeof(app->auth_failure_msg),
                 "Connect failed. Wi-Fi + iTLS? Check auth.json path.");
        app->cspot_connecting = false;
        app->auth_failed = true;
        g_ctx.reset();
        g_spircHandler.reset();
        g_player.reset();
        return 0;
    } catch (...) {
        CSPOT_LOG(error, "start_cspot failed: unknown error");
        snprintf(app->auth_failure_msg, sizeof(app->auth_failure_msg),
                 "Unexpected error. See ux0:data/vitaspotify/log.txt");
        app->cspot_connecting = false;
        app->auth_failed = true;
        g_ctx.reset();
        g_spircHandler.reset();
        g_player.reset();
        return 0;
    }

    g_spircHandler->setEventHandler([app](std::unique_ptr<cspot::SpircHandler::Event> event) {
        switch (event->eventType) {
            case cspot::SpircHandler::EventType::TRACK_INFO: {
                auto track = std::get<cspot::TrackInfo>(event->data);
                CSPOT_LOG(info, "Track: %s / %s", track.name.c_str(),
                          track.artist.c_str());
                {
                    std::lock_guard<std::mutex> lock(app->ui_mutex);
                    app->track_name = track.name;
                    app->track_artist = track.artist;
                    app->track_album = track.album;
                    app->track_image_url = track.imageUrl;
                    if (track.duration > 0) {
                        app->track_duration_ms = track.duration;
                    }
                }
                app->track_dirty = true;
                break;
            }
            case cspot::SpircHandler::EventType::PLAY_PAUSE: {
                const bool paused = std::get<bool>(event->data);
                app->pending_play_pause = paused ? 1 : 0;
                break;
            }
            case cspot::SpircHandler::EventType::FLUSH:
            case cspot::SpircHandler::EventType::DISC:
            case cspot::SpircHandler::EventType::SEEK:
                app->pending_flush = true;
                break;
            case cspot::SpircHandler::EventType::PLAYBACK_START:
                app->pending_playback_start = true;
                break;
            case cspot::SpircHandler::EventType::DEPLETED:
                app->pending_depleted = true;
                break;
            case cspot::SpircHandler::EventType::VOLUME:
                app->pending_volume = std::get<int>(event->data);
                break;
            default:
                break;
        }
    });

    app->playLocalCallback = [app](const std::string &contextUri,
                                   const std::vector<std::string> &trackUris,
                                   int startIndex) {
        app->stop_offline_playback();
        app->local_playback = false;
        std::lock_guard<std::mutex> lock(g_cspot_mutex);
        if (!g_spircHandler) {
            return;
        }
        g_spircHandler->loadAndPlayTracks(trackUris, contextUri, startIndex);
        g_playbackPaused = false;
        app->is_paused = false;
        app->pending_play_pause = 0;
        app->track_dirty = true;
        if (g_player) {
            g_player->onPlayPause(false);
        }
    };

    g_ctx->session->startTask();

    while (app->isRunning && !app->request_disconnect.load()) {
        try {
            g_ctx->session->handlePacket();
        } catch (const std::exception &e) {
            CSPOT_LOG(error, "handlePacket: %s", e.what());
            snprintf(app->auth_failure_msg, sizeof(app->auth_failure_msg),
                     "Spotify connection lost.");
            app->auth_failed = true;
            app->isRunning = false;
            break;
        } catch (...) {
            CSPOT_LOG(error, "handlePacket: unknown error");
            app->isRunning = false;
            break;
        }
        sceKernelDelayThread(10000);
    }

    if (app->request_disconnect.load()) {
        CSPOT_LOG(info, "Disconnecting Spotify session (offline mode)");
    }

    app->cspot_started = false;
    app->player_create_pending = false;
    return 0;
}

void start_cspot_thread(App *app) {
    app->player_create_pending = false;
    app->pending_play_pause = -1;
    app->pending_flush = false;
    app->pending_playback_start = false;
    app->pending_depleted = false;
    app->pending_volume = -1;

    // Force any prior session's thread to wind down before we wait on it, then
    // clear the flag so the fresh session keeps running.
    app->request_disconnect = true;
    stop_cspot_thread();
    app->request_disconnect = false;
    cspot_id = sceKernelCreateThread("cspot", (SceKernelThreadEntry)start_cspot,
                                     0x10000100, 0x80000, 0, 0, nullptr);
    if (cspot_id < 0) {
        snprintf(app->auth_failure_msg, sizeof(app->auth_failure_msg),
                 "Could not start network thread.");
        app->cspot_connecting = false;
        app->auth_failed = true;
        return;
    }
    App *app_ptr = app;
    sceKernelStartThread(cspot_id, sizeof(void *), &app_ptr);
}

int main(void) {
    sceIoMkdir("ux0:data/vitaspotify", 0777);
    sceIoMkdir("ux0:data/vitaspotify/cache", 0777);
    OfflineStore::ensureDir();

    init_logger();
    bell::enableTimestampLogging();
    CSPOT_LOG(info, "VitaSpotify starting");

    init_app_util();

    App app;
    init_platform();
    init_network();

    App *app_p = &app;

    quit_listener_id =
        sceKernelCreateThread("quit_listener", (SceKernelThreadEntry)quit_listener,
                              0x10000100, 0x4000, 0, 0, nullptr);
    if (quit_listener_id >= 0) {
        sceKernelStartThread(quit_listener_id, sizeof(void *), &app_p);
    }

    watch_id = sceKernelCreateThread("watchdog", (SceKernelThreadEntry)watch_dog,
                                     0x10000100, 0x4000, 0, 0, nullptr);
    sceKernelStartThread(watch_id, sizeof(void *), &app_p);

    g_file = std::make_shared<CliFile>();
    g_loginBlob = std::make_shared<cspot::LoginBlob>(DEVICE_NAME);

    app.init();
    app.run();

    app.prepare_exit();
    CSPOT_LOG(info, "Shutting down");

    vitaspotify_release_all_audio_resources();
    stop_cspot_thread();

    if (quit_listener_id >= 0) {
        sceKernelWaitThreadEnd(quit_listener_id, nullptr, nullptr);
        sceKernelDeleteThread(quit_listener_id);
        quit_listener_id = -1;
    }

    if (watch_id >= 0) {
        sceKernelWaitThreadEnd(watch_id, nullptr, nullptr);
        sceKernelDeleteThread(watch_id);
        watch_id = -1;
    }

    app.shutdown();

    flush_logger();
    vitaspotify_release_all_audio_resources();
    term_network();
    term_platform();

    flush_logger();
    sceKernelExitProcess(0);
    return 0;
}
