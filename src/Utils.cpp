#include "Utils.h"
#include <curl/curl.h>
#include <cstring>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <Logger.h>
#include "Config.h"

struct MemoryStruct {
  char *memory;
  size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = (char *) realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        CSPOT_LOG(error, "not enough memory (realloc returned NULL)");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

std::string cover_art_path(std::string url) {
    size_t found = url.find_last_of("/\\");
    std::string path = "ux0:data/vitaspotify/cache/" + url.substr(found+1);
    return path;
}

bool is_cover_cached(std::string url) {
    SceIoStat stat;
    std::string path = cover_art_path(url);
    return sceIoGetstat(path.c_str(), &stat) == 0;
}

bool cache_cover_art(std::string url, uint8_t *buffer, uint32_t length) {
    std::string path = cover_art_path(url);
    int fd = sceIoOpen(path.c_str(), SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0666);
    if (fd < 0) {
        return false;
    }

    sceIoWrite(fd, buffer, length);
    sceIoClose(fd);
    return true;
}

static int XferInfoCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto *fn = static_cast<const DownloadProgressFn *>(clientp);
    if (fn && *fn) {
        (*fn)(static_cast<size_t>(dlnow < 0 ? 0 : dlnow),
              static_cast<size_t>(dltotal < 0 ? 0 : dltotal));
    }
    return 0;
}

int download(const char *url, uint8_t **return_buffer, const char *method, std::string post_data, Headers headers, const DownloadProgressFn &progress) {
    CURL *curl_handle;
    CURLcode res;

    struct MemoryStruct chunk;
    chunk.memory = (char *) malloc(1);
    chunk.size = 0;

    curl_handle = curl_easy_init();
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, method);

    if (progress) {
        curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
        curl_easy_setopt(curl_handle, CURLOPT_XFERINFODATA, (void *)&progress);
    }

    if (post_data.size() != 0) {
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, -1L);
    }

    struct curl_slist *headerchunk = NULL;
    for (auto it : headers) {
        headerchunk = curl_slist_append(headerchunk, it.c_str());
    }
    res = curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headerchunk);

    // Perform the request
    res = curl_easy_perform(curl_handle);
    int httpresponsecode = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &httpresponsecode);
    if (httpresponsecode != 200) {
        CSPOT_LOG(debug, "response code: %d", httpresponsecode);
    }

    if (res != CURLE_OK) {
        CSPOT_LOG(error, "curl_easy_perform() failed: %s", curl_easy_strerror(res));
        free(chunk.memory);
        *return_buffer = NULL;
        chunk.size = 0;
    } else {
        CSPOT_LOG(debug, "%d bytes retrieved", (int)chunk.size);
        *return_buffer = (uint8_t *) chunk.memory;
    }
    curl_easy_cleanup(curl_handle);
    return chunk.size;
}

bool init_platform() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    return true;
}

void term_platform() {
}

int is_dir(const char *path) {
    SceIoStat stat = {0};
    if (sceIoGetstat(path, &stat) < 0) {
        return 0;
    }
    return SCE_S_ISDIR(stat.st_mode);
}

bool init_network() {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    int res;

    SceNetInitParam net_init_param;
    net_init_param.size = 0x200000;
    net_init_param.flags = 0;

    SceUID memid = sceKernelAllocMemBlock("SceNetMemory", 0x0C20D060, net_init_param.size, NULL);
    if (memid < 0) {
        CSPOT_LOG(error, "sceKernelAllocMemBlock failed (0x%X)\n", memid);
        return false;
    }

    sceKernelGetMemBlockBase(memid, &net_init_param.memory);

    res = sceNetInit(&net_init_param);
    if (res < 0) {
        CSPOT_LOG(error, "sceNetInit failed (0x%X)\n", res);
        return false;
    }

    res = sceNetCtlInit();
    if (res < 0) {
        CSPOT_LOG(error, "sceNetCtlInit failed (0x%X)\n", res);
        return false;
    }

    return true;
}

void term_network() {
    sceNetCtlTerm();
    sceNetTerm();
}
