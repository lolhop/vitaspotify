#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "AudioSink.h"
#include "BellTask.h"

namespace bell {
class CentralAudioBuffer;
}

namespace cspot {
class SpircHandler;
}

class VitaPlayer : public bell::Task {
 public:
    VitaPlayer(std::shared_ptr<AudioSink> sink,
               std::shared_ptr<cspot::SpircHandler> handler);
    void disconnect();
    // Stop streaming without releasing the shared audio port (mode switches).
    void stopPlayback();
    // Stop the player task immediately without releasing the shared audio port.
    void forceStop();

    void onPlayPause(bool paused);
    void onFlush();
    void onPlaybackStart();
    void onDepleted();
    void onVolume(int volume);

 private:
    void runTask() override;

    std::shared_ptr<AudioSink> audioSink;
    std::shared_ptr<cspot::SpircHandler> handler;
    std::shared_ptr<bell::CentralAudioBuffer> centralAudioBuffer;

    std::atomic<bool> isPaused{false};
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> isRunning{true};
    std::atomic<bool> playlistEnd{false};
    std::mutex runningMutex;
};
