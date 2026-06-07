#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Utils.h"

namespace cspot {
class CDNAudioFile;
}

struct OfflineTrackEntry {
    std::string path;
    std::string title;
    std::string artist;
    std::string filename;
    std::string coverPath;
    std::string playlist;  // subfolder name; empty = loose root download
    std::string uri;       // Spotify track URI (empty for older downloads)
};

namespace OfflineStore {

// In every call, an empty `playlist` means the loose root folder
// (ux0:data/vitaspotify/dl/). A non-empty value is a subfolder
// (ux0:data/vitaspotify/dl/<playlist>/) so downloads stay grouped on disk.
bool ensureDir(const std::string& playlist = "");
std::string dirForPlaylist(const std::string& playlist);

bool saveDecryptedOgg(std::shared_ptr<cspot::CDNAudioFile> audio,
                      const std::string& title, const std::string& artist,
                      std::string& outPath, std::string& errMsg);
// Fetches the encrypted CDN file with curl and decrypts it with AES-CTR.
// Avoids opening a second bell HTTP/TLS stream alongside the live cspot
// session (that concurrency is what crashes the Vita).
bool saveCdnTrack(const std::string& cdnUrl,
                  const std::vector<uint8_t>& audioKey, const std::string& title,
                  const std::string& artist, const std::string& playlist,
                  const std::string& uri, std::string& outPath,
                  std::string& errMsg,
                  const DownloadProgressFn& progress = nullptr);
// All tracks in one folder (playlist=="" -> loose root only).
void listTracks(std::vector<OfflineTrackEntry>& out,
                const std::string& playlist = "");
// Every downloaded track across the root and all playlist subfolders.
void listAllTracks(std::vector<OfflineTrackEntry>& out);
// Names of playlist subfolders that contain at least one downloaded track.
void listPlaylists(std::vector<std::string>& out);
// Set of Spotify track URIs already downloaded into a playlist subfolder.
void downloadedUris(const std::string& playlist,
                    std::vector<std::string>& out);
bool readMeta(const std::string& oggPath, std::string& title,
              std::string& artist);
std::string pathForTitle(const std::string& title,
                         const std::string& playlist = "");
std::string coverPathForTitle(const std::string& title,
                              const std::string& playlist = "");
std::string coverPathForOgg(const std::string& oggPath);
bool hasDownload(const std::string& title, const std::string& playlist = "");
// True if the track exists in the root or any playlist subfolder.
bool hasDownloadAnywhere(const std::string& title);
bool hasCoverForOgg(const std::string& oggPath);
bool coverFileExists(const std::string& coverPath);
bool saveCoverFromUrl(const std::string& title, const std::string& imageUrl,
                      const std::string& playlist = "");
void abortWriteFile(FILE* file, const std::string& path);

}  // namespace OfflineStore
