#include "LocalPlayer.h"

#include "BellUtils.h"
#include "Logger.h"
#include "VitaAudioSink.h"
#include "VitaPlayer.h"

#include <ivorbisfile.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::shared_ptr<VitaAudioSink> g_sharedAudioSink;
std::shared_ptr<LocalPlayer> g_localPlayer;
}  // namespace

extern std::shared_ptr<VitaPlayer> g_player;

void vitaspotify_ensure_local_audio() {
    if (g_sharedAudioSink && !g_sharedAudioSink->isAlive()) {
        if (g_localPlayer) {
            g_localPlayer->forceStop();
            g_localPlayer.reset();
        }
        g_sharedAudioSink.reset();
    }
    if (!g_sharedAudioSink) {
        g_sharedAudioSink = std::make_shared<VitaAudioSink>();
    }
    if (!g_localPlayer) {
        g_localPlayer = std::make_shared<LocalPlayer>(g_sharedAudioSink);
    }
}

std::shared_ptr<VitaAudioSink> vitaspotify_get_audio_sink() {
    vitaspotify_ensure_local_audio();
    return g_sharedAudioSink;
}

std::shared_ptr<LocalPlayer> vitaspotify_get_local_player() {
    vitaspotify_ensure_local_audio();
    return g_localPlayer;
}

void vitaspotify_emergency_stop_playback() {
    if (g_player) {
        g_player->forceStop();
    }
    if (g_localPlayer) {
        g_localPlayer->forceStop();
    }
}

void vitaspotify_force_release_audio() {
    vitaspotify_emergency_stop_playback();
    if (g_sharedAudioSink) {
        g_sharedAudioSink->forceRelease();
    }
    g_localPlayer.reset();
    g_sharedAudioSink.reset();
}

void vitaspotify_shutdown_local_audio() {
    vitaspotify_force_release_audio();
}

LocalPlayer::LocalPlayer(std::shared_ptr<VitaAudioSink> sink)
    : bell::Task("local_player", 1024 * 48, 2, 1), audioSink(std::move(sink)) {
    startTask();
}

LocalPlayer::~LocalPlayer() {
    stop();
    isRunning = false;
    std::scoped_lock lock(runningMutex);
}

void LocalPlayer::stop() {
    stopRequested = true;
    active = false;
    reopen = true;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath.clear();
    }
}

void LocalPlayer::stopAndClear() {
    paused = true;
    stop();
    for (int i = 0; i < 100 && pcmBusy.load(); ++i) {
        BELL_SLEEP_MS(5);
    }
    if (audioSink) {
        audioSink->clearBufferedAudio();
    }
}

void LocalPlayer::forceStop() {
    stop();
    isRunning = false;
}

bool LocalPlayer::consumeTrackEnded() {
    return trackEnded.exchange(false);
}

bool LocalPlayer::play(const std::string& oggPath) {
    stopRequested = false;
    trackEnded = false;
    {
        std::lock_guard<std::mutex> lock(pathMutex);
        currentPath = oggPath;
    }
    paused = false;
    active = true;
    reopen = true;
    hasPendingSeek = false;
    return true;
}

void LocalPlayer::setPaused(bool p) {
    paused = p;
}

void LocalPlayer::seekMs(uint32_t ms) {
    pendingSeekMs = ms;
    hasPendingSeek = true;
}

void LocalPlayer::closeFile() {
    // File handle owned by ov_clear when vf was opened.
}

void LocalPlayer::runTask() {
    std::scoped_lock lock(runningMutex);
    std::vector<int16_t> pcm(4096 * 2);
    OggVorbis_File vf{};
    FILE* file = nullptr;
    bool open = false;

    while (isRunning) {
        if (reopen.exchange(false) || !open) {
            if (open) {
                ov_clear(&vf);
                open = false;
            }
            if (file) {
                fclose(file);
                file = nullptr;
            }
            durationMs = 0;
            positionMs = 0;

            std::string path;
            {
                std::lock_guard<std::mutex> pl(pathMutex);
                path = currentPath;
            }
            if (!active.load() || path.empty() || stopRequested.load()) {
                BELL_SLEEP_MS(50);
                continue;
            }

            file = fopen(path.c_str(), "rb");
            if (!file) {
                CSPOT_LOG(error, "LocalPlayer: cannot open %s", path.c_str());
                active = false;
                continue;
            }
            if (ov_open(file, &vf, nullptr, 0) < 0) {
                CSPOT_LOG(error, "LocalPlayer: ov_open failed");
                fclose(file);
                file = nullptr;
                active = false;
                continue;
            }
            open = true;
            const long totalMs = ov_time_total(&vf, -1);
            if (totalMs > 0) {
                durationMs = static_cast<uint32_t>(totalMs);
            }
            CSPOT_LOG(info, "LocalPlayer: playing %s (%ld ms)", path.c_str(),
                      totalMs);
        }

        if (!active.load() || !open || stopRequested.load()) {
            BELL_SLEEP_MS(50);
            continue;
        }

        if (hasPendingSeek.exchange(false)) {
            const uint32_t ms = pendingSeekMs.load();
            ov_time_seek(&vf, static_cast<long>(ms));
            positionMs = ms;
        }

        if (paused.load()) {
            const long t = ov_time_tell(&vf);
            if (t >= 0) {
                positionMs = static_cast<uint32_t>(t);
            }
            BELL_SLEEP_MS(20);
            continue;
        }

        pcmBusy = true;
        int section = 0;
        long ret = ov_read(&vf, reinterpret_cast<char*>(pcm.data()),
                         static_cast<int>(pcm.size() * sizeof(int16_t)),
                         &section);
        if (ret == 0) {
            pcmBusy = false;
            trackEnded = true;
            active = false;
            paused = true;
            if (open) {
                ov_clear(&vf);
                open = false;
            }
            if (file) {
                fclose(file);
                file = nullptr;
            }
            BELL_SLEEP_MS(50);
            continue;
        }
        if (ret < 0) {
            pcmBusy = false;
            reopen = true;
            continue;
        }

        const long t = ov_time_tell(&vf);
        if (t >= 0) {
            positionMs = static_cast<uint32_t>(t);
        }

        if (audioSink && active.load() && !stopRequested.load()) {
            audioSink->feedPCMFrames(reinterpret_cast<uint8_t*>(pcm.data()),
                                     static_cast<size_t>(ret));
        }
        pcmBusy = false;
    }

    if (open) {
        ov_clear(&vf);
    }
    if (file) {
        fclose(file);
    }
}
