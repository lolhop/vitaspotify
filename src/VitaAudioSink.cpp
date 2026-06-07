#include "VitaAudioSink.h"
#include "VitaRingBuffer.h"
#include "Logger.h"

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <cstring>

#define ONE_BUFFER_SIZE       4096
#define VITA_DECODE_SIZE      (ONE_BUFFER_SIZE / 4)

namespace {
std::atomic<int> g_liveAudioPort{-1};
std::atomic<int> g_liveAudioThread{-1};
}  // namespace

int VitaAudioSink::audioThreadEntry(SceSize /*args*/, void *argp) {
    auto *self = *static_cast<VitaAudioSink **>(argp);
    self->audioThreadLoop();
    return 0;
}

void VitaAudioSink::audioThreadLoop() {
    uint8_t current_buffer[ONE_BUFFER_SIZE];
    while (end_flag == 0) {
        if (ring_buffer.size() >= ONE_BUFFER_SIZE) {
            auto readNumber = ring_buffer.read(current_buffer, ONE_BUFFER_SIZE);
            if (readNumber != ONE_BUFFER_SIZE) {
                CSPOT_LOG(error, "buffer error");
            }

            int res = sceAudioOutOutput(port, current_buffer);
            if (res < 0) {
                CSPOT_LOG(error, "sceAudioOutOutput error");
            }
        } else {
            sceKernelDelayThread(10000);
        }
    }
}

VitaAudioSink::VitaAudioSink() {
    softwareVolumeControl = false;
    end_flag = 0;
    ring_buffer.emptyBuffer();

    sceAppMgrReleaseBgmPort();
    sceAppMgrAcquireBgmPort();

    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, VITA_DECODE_SIZE, 44100,
                               SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        CSPOT_LOG(error, "BGM audio port failed: 0x%X, trying MAIN", port);
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VITA_DECODE_SIZE, 44100,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    }
    if (port < 0) {
        CSPOT_LOG(error, "sceAudioOutOpenPort failed: 0x%X", port);
        return;
    }
    CSPOT_LOG(info, "Audio port opened: %d", port);

    int max_vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port,
                         (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                                  SCE_AUDIO_VOLUME_FLAG_R_CH),
                         max_vol);
    sceAudioOutOutput(port, nullptr);

    threadid = sceKernelCreateThread("audio output",
                                     (SceKernelThreadEntry)audioThreadEntry,
                                     0x10000100, 0x4000, 0, 0, nullptr);
    VitaAudioSink *self = this;
    sceKernelStartThread(threadid, sizeof(VitaAudioSink *), &self);
    g_liveAudioPort.store(port);
    g_liveAudioThread.store(threadid);
}

bool VitaAudioSink::isAlive() const {
    return port >= 0 && end_flag == 0;
}

void VitaAudioSink::clearBufferedAudio() {
    ring_buffer.emptyBuffer();
}

void VitaAudioSink::forceRelease() {
    end_flag = 1;
    if (port >= 0) {
        sceAudioOutReleasePort(port);
        port = -1;
    }
    g_liveAudioPort.store(-1);
    g_liveAudioThread.store(-1);
}

void VitaAudioSink::emergencyKillAll() {
    g_liveAudioThread.store(-1);
    const int livePort = g_liveAudioPort.exchange(-1);
    if (livePort >= 0) {
        sceAudioOutReleasePort(livePort);
    }
    sceAppMgrReleaseBgmPort();
}

VitaAudioSink::~VitaAudioSink() {
    end_flag = 1;
    if (threadid >= 0) {
        sceKernelWaitThreadEnd(threadid, nullptr, nullptr);
        sceKernelDeleteThread(threadid);
        threadid = -1;
    }
    if (port >= 0) {
        sceAudioOutReleasePort(port);
        port = -1;
    }
}

void VitaAudioSink::feedPCMFrames(const uint8_t *buf, size_t bytes) {
    size_t bytesWritten = 0;
    while (bytesWritten < bytes) {
        auto bwrite = ring_buffer.write(buf + bytesWritten, bytes - bytesWritten);
        bytesWritten += bwrite;

        if (bwrite == 0) {
            sceKernelDelayThread(10000);
        }
    }
}

void VitaAudioSink::volumeChanged(uint16_t volume) {
    if (port < 0) {
        return;
    }
    // Spotify volume is 0..65535; map to Vita 0..SCE_AUDIO_VOLUME_0DB
    uint32_t spotify_vol = volume;
    if (spotify_vol == 0) {
        spotify_vol = 65535 / 2;
    }
    int hw_vol = static_cast<int>(
        (static_cast<uint64_t>(spotify_vol) * SCE_AUDIO_VOLUME_0DB) / 65535);
    if (hw_vol > SCE_AUDIO_VOLUME_0DB) {
        hw_vol = SCE_AUDIO_VOLUME_0DB;
    }
    int vol[2] = {hw_vol, hw_vol};
    sceAudioOutSetVolume(port,
                         (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                                  SCE_AUDIO_VOLUME_FLAG_R_CH),
                         vol);
}
