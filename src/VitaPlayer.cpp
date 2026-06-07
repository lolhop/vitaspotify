#include "VitaPlayer.h"

#include <functional>
#include <string_view>

#include "Logger.h"
#include "BellUtils.h"
#include "CentralAudioBuffer.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "VitaAudioSink.h"

VitaPlayer::VitaPlayer(std::shared_ptr<AudioSink> sink,
                       std::shared_ptr<cspot::SpircHandler> handler)
    : bell::Task("vitaspotify_player", 1024 * 24, 0, 0) {
    this->handler = handler;
    this->audioSink = std::move(sink);
    this->audioSink->setParams(44100, 2, 16);

    // ~48 PCM chunks (~192 KiB). 128*1024 overflows Vita RAM (bad_alloc).
    centralAudioBuffer = std::make_shared<bell::CentralAudioBuffer>(48);

    handler->getTrackPlayer()->setDataCallback(
        [this](uint8_t* data, size_t bytes, std::string_view trackId) {
            const auto hash = std::hash<std::string_view>{}(trackId);
            return centralAudioBuffer->writePCM(data, bytes, hash);
        });

    startTask();
}

void VitaPlayer::onPlayPause(bool paused) {
    if (paused) {
        pauseRequested = true;
    } else {
        isPaused = false;
        pauseRequested = false;
    }
}

void VitaPlayer::onFlush() {
    centralAudioBuffer->clearBuffer();
}

void VitaPlayer::onPlaybackStart() {
    playlistEnd = false;
    centralAudioBuffer->clearBuffer();
}

void VitaPlayer::onDepleted() {
    playlistEnd = true;
}

void VitaPlayer::onVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    }
    audioSink->volumeChanged(static_cast<uint16_t>(volume));
}

void VitaPlayer::runTask() {
    std::scoped_lock lock(runningMutex);
    size_t lastHash = 0;

    while (isRunning) {
        if (!isPaused.load()) {
            auto* chunk = centralAudioBuffer->readChunk();

            if (pauseRequested.exchange(false)) {
                isPaused = true;
            }

            if (!chunk || chunk->pcmSize == 0) {
                if (playlistEnd.exchange(false)) {
                    handler->notifyAudioEnded();
                }
                BELL_SLEEP_MS(10);
                continue;
            }

            if (lastHash != chunk->trackHash) {
                lastHash = chunk->trackHash;
                handler->notifyAudioReachedPlayback();
            }

            static int pcm_log_count = 0;
            if (pcm_log_count < 5) {
                CSPOT_LOG(info, "PCM to speaker: %d bytes", (int)chunk->pcmSize);
                pcm_log_count++;
            }
            audioSink->feedPCMFrames(chunk->pcmData, chunk->pcmSize);
        } else {
            BELL_SLEEP_MS(10);
        }
    }
}

void VitaPlayer::stopPlayback() {
    isRunning = false;
    pauseRequested = true;
    isPaused = true;
    if (centralAudioBuffer) {
        centralAudioBuffer->clearBuffer();
    }
    // runTask holds runningMutex for its whole lifetime; wait here so callers
    // can safely destroy VitaPlayer (bell::Task uses a detached pthread).
    {
        std::scoped_lock lock(runningMutex);
    }
    if (audioSink) {
        auto *vitaSink = static_cast<VitaAudioSink *>(audioSink.get());
        if (vitaSink->isAlive()) {
            vitaSink->clearBufferedAudio();
        }
    }
}

void VitaPlayer::forceStop() {
    stopPlayback();
}

void VitaPlayer::disconnect() {
    stopPlayback();
}
