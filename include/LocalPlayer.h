#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "BellTask.h"

class VitaAudioSink;

// Plays a decrypted .ogg file from ux0:data/vitaspotify/dl/ to the shared sink.
class LocalPlayer : public bell::Task {
 public:
    explicit LocalPlayer(std::shared_ptr<VitaAudioSink> sink);
    ~LocalPlayer();

    bool play(const std::string& oggPath);
    void stop();
    void stopAndClear();
    void forceStop();
    bool isActive() const { return active.load(); }
    void setPaused(bool paused);
    bool isPaused() const { return paused.load(); }

    void seekMs(uint32_t ms);
    uint32_t getPositionMs() const { return positionMs.load(); }
    uint32_t getDurationMs() const { return durationMs.load(); }
    bool consumeTrackEnded();

 private:
    void runTask() override;
    void closeFile();

    std::shared_ptr<VitaAudioSink> audioSink;
    std::string currentPath;
    std::mutex pathMutex;
    std::atomic<bool> active{false};
    std::atomic<bool> paused{true};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> reopen{false};
    std::atomic<bool> isRunning{true};
    std::atomic<uint32_t> pendingSeekMs{0};
    std::atomic<bool> hasPendingSeek{false};
    std::atomic<uint32_t> positionMs{0};
    std::atomic<uint32_t> durationMs{0};
    std::atomic<bool> trackEnded{false};
    std::atomic<bool> pcmBusy{false};
    std::mutex runningMutex;
};

void vitaspotify_ensure_local_audio();
void vitaspotify_shutdown_local_audio();
void vitaspotify_force_release_audio();
void vitaspotify_emergency_stop_playback();
std::shared_ptr<VitaAudioSink> vitaspotify_get_audio_sink();
std::shared_ptr<LocalPlayer> vitaspotify_get_local_player();
