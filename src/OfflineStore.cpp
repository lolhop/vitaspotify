#include "OfflineStore.h"

#include "Config.h"
#include "Crypto.h"
#include "Logger.h"
#include "Utils.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Spotify prefixes each CDN audio file with a fixed header; the AES-CTR
// keystream (constant IV below) covers the whole file from byte 0.
constexpr size_t kSpotifyOpusHeader = 167;
const unsigned char kAudioAesIvBytes[16] = {0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb,
                                            0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64,
                                            0x3f, 0x63, 0x0d, 0x93};

std::string sanitizeBaseName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            out.push_back('_');
        } else if (c < 32) {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    if (out.empty()) {
        out = "track";
    }
    if (out.size() > 48) {
        out.resize(48);
    }
    return out;
}

void writeMetaLine(FILE* f, const char* key, const std::string& value) {
    fputs(key, f);
    fputs(value.c_str(), f);
    fputc('\n', f);
}

bool writeMetaFile(const std::string& oggPath, const std::string& title,
                   const std::string& artist, const std::string& uri) {
    const std::string metaPath = oggPath + ".meta";
    FILE* f = fopen(metaPath.c_str(), "w");
    if (!f) {
        return false;
    }
    writeMetaLine(f, "title=", title);
    writeMetaLine(f, "artist=", artist);
    if (!uri.empty()) {
        writeMetaLine(f, "uri=", uri);
    }
    fclose(f);
    return true;
}

}  // namespace

namespace OfflineStore {

std::string dirForPlaylist(const std::string& playlist) {
    if (playlist.empty()) {
        return std::string(OFFLINE_DL_DIR);
    }
    return std::string(OFFLINE_DL_DIR) + sanitizeBaseName(playlist) + "/";
}

std::string pathForTitle(const std::string& title, const std::string& playlist) {
    return dirForPlaylist(playlist) + sanitizeBaseName(title) + ".ogg";
}

std::string coverPathForTitle(const std::string& title,
                              const std::string& playlist) {
    return dirForPlaylist(playlist) + sanitizeBaseName(title) + ".jpg";
}

std::string coverPathForOgg(const std::string& oggPath) {
    if (oggPath.size() >= 4 && oggPath.compare(oggPath.size() - 4, 4, ".ogg") == 0) {
        return oggPath.substr(0, oggPath.size() - 4) + ".jpg";
    }
    return oggPath + ".jpg";
}

bool ensureDir(const std::string& playlist) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/vitaspotify", 0777);
    sceIoMkdir(OFFLINE_DL_DIR, 0777);
    const std::string dir = dirForPlaylist(playlist);
    SceIoStat st{};
    return sceIoMkdir(dir.c_str(), 0777) >= 0 ||
           sceIoGetstat(dir.c_str(), &st) >= 0;
}

bool saveCdnTrack(const std::string& cdnUrl,
                  const std::vector<uint8_t>& audioKey, const std::string& title,
                  const std::string& artist, const std::string& playlist,
                  const std::string& uri, std::string& outPath,
                  std::string& errMsg, const DownloadProgressFn& progress) {
    outPath.clear();
    errMsg.clear();
    if (cdnUrl.empty() || audioKey.empty()) {
        errMsg = "no stream";
        return false;
    }
    ensureDir(playlist);

    CSPOT_LOG(info, "saveCdnTrack: starting curl GET (key %d bytes)",
              (int)audioKey.size());
    uint8_t* buf = nullptr;
    const int len = download(cdnUrl.c_str(), &buf, "GET", "", {}, progress);
    CSPOT_LOG(info, "saveCdnTrack: curl returned len=%d buf=%p", len,
              static_cast<void*>(buf));
    if (len <= static_cast<int>(kSpotifyOpusHeader) || buf == nullptr) {
        if (buf) {
            free(buf);
        }
        errMsg = "cdn download failed";
        return false;
    }

    try {
        Crypto crypto;
        std::vector<uint8_t> iv(kAudioAesIvBytes,
                                kAudioAesIvBytes + sizeof(kAudioAesIvBytes));
        crypto.aesCTRXcrypt(audioKey, iv, buf, static_cast<size_t>(len));
    } catch (...) {
        free(buf);
        errMsg = "decrypt failed";
        return false;
    }
    CSPOT_LOG(info, "saveCdnTrack: decrypt done");

    const std::string path = pathForTitle(title, playlist);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        free(buf);
        errMsg = "cannot create file";
        return false;
    }
    const size_t bodyLen = static_cast<size_t>(len) - kSpotifyOpusHeader;
    const bool ok = fwrite(buf + kSpotifyOpusHeader, 1, bodyLen, f) == bodyLen;
    fclose(f);
    free(buf);
    CSPOT_LOG(info, "saveCdnTrack: file write ok=%d", ok ? 1 : 0);
    if (!ok) {
        sceIoRemove(path.c_str());
        errMsg = "disk write error";
        return false;
    }

    writeMetaFile(path, title, artist, uri);
    outPath = path;
    CSPOT_LOG(info, "saveCdnTrack: wrote %d bytes to %s", (int)bodyLen,
              path.c_str());
    return true;
}

namespace {
bool readMetaFields(const std::string& oggPath, std::string& title,
                    std::string& artist, std::string& uri) {
    title.clear();
    artist.clear();
    uri.clear();
    const std::string metaPath = oggPath + ".meta";
    FILE* f = fopen(metaPath.c_str(), "r");
    if (!f) {
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (strncmp(line, "title=", 6) == 0) {
            title = line + 6;
        } else if (strncmp(line, "artist=", 7) == 0) {
            artist = line + 7;
        } else if (strncmp(line, "uri=", 4) == 0) {
            uri = line + 4;
        }
    }
    fclose(f);
    return !title.empty();
}
}  // namespace

bool readMeta(const std::string& oggPath, std::string& title,
              std::string& artist) {
    std::string uri;
    return readMetaFields(oggPath, title, artist, uri);
}

namespace {

void appendTracksFromDir(const std::string& dir, const std::string& playlist,
                         std::vector<OfflineTrackEntry>& out) {
    SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) {
        return;
    }
    SceIoDirent entry{};
    while (sceIoDread(d, &entry) > 0) {
        const char* name = entry.d_name;
        const size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".ogg") != 0) {
            continue;
        }
        OfflineTrackEntry e;
        e.filename = name;
        e.path = dir + name;
        e.playlist = playlist;
        if (!readMetaFields(e.path, e.title, e.artist, e.uri)) {
            e.title = name;
            e.title.resize(len - 4);
            e.artist = "";
        }
        e.coverPath = coverPathForOgg(e.path);
        out.push_back(std::move(e));
    }
    sceIoDclose(d);
}

void sortByTitle(std::vector<OfflineTrackEntry>& out) {
    std::sort(out.begin(), out.end(),
              [](const OfflineTrackEntry& a, const OfflineTrackEntry& b) {
                  return a.title < b.title;
              });
}

}  // namespace

void listTracks(std::vector<OfflineTrackEntry>& out,
                const std::string& playlist) {
    out.clear();
    ensureDir(playlist);
    appendTracksFromDir(dirForPlaylist(playlist), playlist, out);
    sortByTitle(out);
}

void listPlaylists(std::vector<std::string>& out) {
    out.clear();
    ensureDir();
    SceUID d = sceIoDopen(OFFLINE_DL_DIR);
    if (d < 0) {
        return;
    }
    SceIoDirent entry{};
    while (sceIoDread(d, &entry) > 0) {
        if (!SCE_S_ISDIR(entry.d_stat.st_mode)) {
            continue;
        }
        const char* name = entry.d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        // Only list folders that actually contain a downloaded track.
        std::vector<OfflineTrackEntry> probe;
        appendTracksFromDir(std::string(OFFLINE_DL_DIR) + name + "/", name,
                            probe);
        if (!probe.empty()) {
            out.push_back(name);
        }
    }
    sceIoDclose(d);
    std::sort(out.begin(), out.end());
}

void downloadedUris(const std::string& playlist,
                    std::vector<std::string>& out) {
    out.clear();
    std::vector<OfflineTrackEntry> tracks;
    listTracks(tracks, playlist);
    for (auto& t : tracks) {
        if (!t.uri.empty()) {
            out.push_back(t.uri);
        }
    }
}

void listAllTracks(std::vector<OfflineTrackEntry>& out) {
    out.clear();
    ensureDir();
    appendTracksFromDir(std::string(OFFLINE_DL_DIR), "", out);
    std::vector<std::string> playlists;
    listPlaylists(playlists);
    for (const auto& pl : playlists) {
        appendTracksFromDir(std::string(OFFLINE_DL_DIR) + sanitizeBaseName(pl) +
                                "/",
                            pl, out);
    }
    sortByTitle(out);
}

bool hasDownload(const std::string& title, const std::string& playlist) {
    if (title.empty()) {
        return false;
    }
    const std::string path = pathForTitle(title, playlist);
    SceIoStat st{};
    return sceIoGetstat(path.c_str(), &st) >= 0;
}

bool hasDownloadAnywhere(const std::string& title) {
    if (title.empty()) {
        return false;
    }
    if (hasDownload(title, "")) {
        return true;
    }
    std::vector<std::string> playlists;
    listPlaylists(playlists);
    for (const auto& pl : playlists) {
        if (hasDownload(title, pl)) {
            return true;
        }
    }
    return false;
}

void abortWriteFile(FILE* file, const std::string& path) {
    if (file) {
        fclose(file);
    }
    if (!path.empty()) {
        sceIoRemove(path.c_str());
        sceIoRemove(coverPathForOgg(path).c_str());
    }
}

bool coverFileExists(const std::string& coverPath) {
    if (coverPath.empty()) {
        return false;
    }
    SceIoStat st{};
    return sceIoGetstat(coverPath.c_str(), &st) >= 0 && st.st_size > 0;
}

bool hasCoverForOgg(const std::string& oggPath) {
    return coverFileExists(coverPathForOgg(oggPath));
}

bool saveCoverFromUrl(const std::string& title, const std::string& imageUrl,
                      const std::string& playlist) {
    if (imageUrl.empty()) {
        return false;
    }
    ensureDir(playlist);

    uint8_t* buf = nullptr;
    const int len = download(imageUrl.c_str(), &buf, "GET", "", {});
    if (len <= 0 || buf == nullptr) {
        if (buf) {
            free(buf);
        }
        return false;
    }

    const std::string path = coverPathForTitle(title, playlist);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        free(buf);
        return false;
    }
    const bool ok =
        fwrite(buf, 1, static_cast<size_t>(len), f) == static_cast<size_t>(len);
    fclose(f);
    free(buf);
    if (!ok) {
        sceIoRemove(path.c_str());
        return false;
    }
    CSPOT_LOG(info, "saveCoverFromUrl: wrote %d bytes to %s", len, path.c_str());
    return true;
}

}  // namespace OfflineStore
