#pragma once

#include <atomic>
#include <psp2/kernel/threadmgr.h>

#include "AudioSink.h"
#include "VitaRingBuffer.h"

class VitaAudioSink : public AudioSink {
 public:
    VitaAudioSink();
    ~VitaAudioSink();
    void feedPCMFrames(const uint8_t *buffer, size_t bytes);
    void volumeChanged(uint16_t volume);
    // Release the audio port without joining the output thread (used when the
    // LiveArea closes the app and we must not block on thread teardown).
    void forceRelease();
    bool isAlive() const;
    void clearBufferedAudio();
    // Stop every live VitaAudioSink output thread/port without blocking.
    static void emergencyKillAll();

 private:
    static constexpr size_t kCircularBufferSize = 4096 * 4;

    static int audioThreadEntry(SceSize args, void *argp);
    void audioThreadLoop();

    int port = -1;
    int threadid = -1;
    int end_flag = 0;
    VitaRingBuffer ring_buffer{kCircularBufferSize};
};
