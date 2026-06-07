#pragma once

#include <psp2/appmgr.h>
#include <psp2/types.h>
#include <functional>
#include <string>
#include <vector>

typedef struct SceAppMgrEvent {
    int event;
    SceUID appId;
    char param[56];
} SceAppMgrEvent;

extern "C" {
    int sceAppMgrReceiveEvent(SceAppMgrEvent *appEvent);
    int sceAppMgrReceiveEventNum(SceInt32 num, SceAppMgrEvent *appEvent, unsigned int *timeout);
    int sceAppMgrQuitForNonSuspendableApp(void);
    SceInt32 sceAppMgrGetAppInfo(const char *unk, SceAppMgrAppState *state);
    SceInt32 sceNotificationUtilBgAppInitialize(void);
}

#define SCE_APP_EVENT_ON_ACTIVATE           (0x10000001)
#define SCE_APP_EVENT_ON_DEACTIVATE         (0x10000002)
#define SCE_APP_EVENT_REQUEST_QUIT          (0x20000001)

typedef std::vector<std::string> Headers;

// Called periodically during a download with (bytesSoFar, totalBytes). total
// may be 0 if the server doesn't report a length yet. Runs on the calling
// thread, so a UI thread can render a progress frame from here.
using DownloadProgressFn = std::function<void(size_t, size_t)>;

int is_dir(const char *path);
bool init_platform();
void term_platform();
bool init_network();
void term_network();
int download(const char *url, uint8_t **return_buffer, const char *method = "GET",
             std::string post_data = "", Headers headers = {},
             const DownloadProgressFn &progress = nullptr);
bool cache_cover_art(std::string url, uint8_t *buffer, uint32_t length);
std::string cover_art_path(std::string url);
bool is_cover_cached(std::string url);
