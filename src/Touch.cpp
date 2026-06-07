#include "Touch.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

namespace Touch {

namespace {
bool g_was_down = false;
int g_last_raw_x = 0;
int g_last_raw_y = 0;
bool g_pending_tap = false;
float g_tap_x = 0;
float g_tap_y = 0;
uint64_t g_last_tap_us = 0;

constexpr uint64_t kTapDebounceUs = 300000;  // 300 ms between accepted taps

uint64_t monotonic_us() {
    return sceKernelGetProcessTimeWide();
}
}  // namespace

void init() {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                            SCE_TOUCH_SAMPLING_STATE_START);
}

void update() {
    SceTouchData touch{};
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

    const bool down = touch.reportNum > 0;
    if (down) {
        g_last_raw_x = touch.report[0].x;
        g_last_raw_y = touch.report[0].y;
    }

    if (g_was_down && !down) {
        const uint64_t now = monotonic_us();
        if (now - g_last_tap_us >= kTapDebounceUs) {
            g_last_tap_us = now;
            g_pending_tap = true;
            g_tap_x = static_cast<float>(g_last_raw_x) * 0.5f;
            g_tap_y = static_cast<float>(g_last_raw_y) * 0.5f;
        }
    }

    g_was_down = down;
}

bool consumeTap(float &x, float &y) {
    if (!g_pending_tap) {
        return false;
    }
    g_pending_tap = false;
    x = g_tap_x;
    y = g_tap_y;
    return true;
}

void discardPendingTap() {
    g_pending_tap = false;
}

bool isDown(float &x, float &y) {
    if (!g_was_down) {
        return false;
    }
    x = static_cast<float>(g_last_raw_x) * 0.5f;
    y = static_cast<float>(g_last_raw_y) * 0.5f;
    return true;
}

}  // namespace Touch
