#include "API.h"
#include "Utils.h"
#include <Logger.h>
#include "Config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <curl/curl.h>

#include "cJSON.h"

#include "TrackReference.h"
#include "NanoPBHelper.h"
#include "pb_decode.h"
#include "protobuf/metadata.pb.h"

void API::set_token(std::string _token) {
    std::lock_guard<std::mutex> lock(token_mutex);
    token = _token;
}

bool API::has_token() {
    std::lock_guard<std::mutex> lock(token_mutex);
    return !token.empty();
}

void API::set_device_id(std::string _device_id) {
    std::lock_guard<std::mutex> lock(token_mutex);
    device_id = _device_id;
}

void API::set_spclient_base(std::string base) {
    std::lock_guard<std::mutex> lock(token_mutex);
    spclient_base = base;
}

void API::set_username(std::string _username) {
    std::lock_guard<std::mutex> lock(token_mutex);
    username = _username;
}

std::string API::liked_songs_uri() {
    std::lock_guard<std::mutex> lock(token_mutex);
    if (username.empty()) {
        return "";
    }
    return "spotify:user:" + username + ":collection";
}

bool API::has_spclient_base() {
    std::lock_guard<std::mutex> lock(token_mutex);
    return !spclient_base.empty();
}

std::string API::get_token() {
    std::lock_guard<std::mutex> lock(token_mutex);
    return token;
}

std::string API::get_spclient_base() {
    std::lock_guard<std::mutex> lock(token_mutex);
    return spclient_base;
}

// Recursively walks a parsed JSON tree collecting any string field named "uri"
// whose value is a playable track/episode URI. This is resilient to the exact
// context-resolve response shape (playlists, albums, search results, pages).
static void collect_track_uris(const cJSON *node,
                               std::vector<std::string> &out,
                               size_t max_tracks) {
    if (node == nullptr || out.size() >= max_tracks) {
        return;
    }

    if (cJSON_IsArray(node) || cJSON_IsObject(node)) {
        for (cJSON *child = node->child; child != nullptr; child = child->next) {
            if (out.size() >= max_tracks) {
                return;
            }
            if (cJSON_IsString(child) && child->string != nullptr &&
                strcmp(child->string, "uri") == 0 && child->valuestring) {
                const std::string uri = child->valuestring;
                if (uri.rfind("spotify:track:", 0) == 0 ||
                    uri.rfind("spotify:episode:", 0) == 0) {
                    out.push_back(uri);
                    continue;
                }
            }
            collect_track_uris(child, out, max_tracks);
        }
    }
}

static std::string url_encode_path(const std::string &in) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        const bool unreserved =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == ':' || c == '+';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

int API::resolve_context_tracks(const std::string &context_uri,
                                std::vector<std::string> &out_uris,
                                size_t max_tracks) {
    std::string token = get_token();
    std::string base = get_spclient_base();
    if (token.empty() || base.empty()) {
        return -1;
    }

    std::string url = base;
    url += "/context-resolve/v1/";
    url += url_encode_path(context_uri);

    Headers headers = { {"Accept: application/json"},
                        {"User-Agent: Spotify/8.6.84 iOS/15.1 (iPhone11,8)"},
                        {"Authorization: Bearer " + token} };

    uint8_t *buf = nullptr;
    int len = download(url.c_str(), &buf, "GET", "", headers);
    if (len <= 0 || buf == nullptr) {
        if (buf) {
            free(buf);
        }
        CSPOT_LOG(error, "context-resolve failed for %s", context_uri.c_str());
        return 0;
    }

    cJSON *root = cJSON_Parse(reinterpret_cast<const char *>(buf));
    free(buf);
    if (root == nullptr) {
        CSPOT_LOG(error, "context-resolve: bad JSON for %s",
                  context_uri.c_str());
        return 0;
    }

    collect_track_uris(root, out_uris, max_tracks);
    cJSON_Delete(root);

    CSPOT_LOG(info, "context-resolve %s -> %d tracks", context_uri.c_str(),
              (int)out_uris.size());
    return static_cast<int>(out_uris.size());
}

namespace {
struct CurlBuffer {
    char *memory;
    size_t size;
};

size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    auto *mem = static_cast<CurlBuffer *>(userp);
    char *ptr = static_cast<char *>(realloc(mem->memory, mem->size + realsize + 1));
    if (!ptr) {
        return 0;
    }
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

// Track GIDs are 128-bit; left-pad the decoded big-endian value to 16 bytes.
std::string gid_hex_from_uri(const std::string &uri) {
    cspot::TrackReference ref;
    ref.uri = uri;
    ref.decodeURI();
    std::vector<uint8_t> gid = ref.gid;
    if (gid.size() < 16) {
        std::vector<uint8_t> padded(16 - gid.size(), 0);
        padded.insert(padded.end(), gid.begin(), gid.end());
        gid = std::move(padded);
    } else if (gid.size() > 16) {
        gid.erase(gid.begin(), gid.begin() + (gid.size() - 16));
    }
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : gid) {
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

std::string id_from_uri(const std::string &uri) {
    const size_t c = uri.find_last_of(':');
    return c == std::string::npos ? uri : uri.substr(c + 1);
}
}  // namespace

int API::fetch_track_results(const std::vector<std::string> &uris,
                             std::vector<TrackResult> &out, size_t max_items) {
    std::string token = get_token();
    std::string base = get_spclient_base();

    const std::string auth = "Authorization: Bearer " + token;
    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/x-protobuf");
    hdrs = curl_slist_append(hdrs, "User-Agent: Spotify/8.6.84 iOS/15.1 (iPhone11,8)");
    hdrs = curl_slist_append(hdrs, auth.c_str());

    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }

    const size_t limit = std::min(uris.size(), max_items);
    for (size_t i = 0; i < limit; i++) {
        const std::string &uri = uris[i];
        TrackResult item;
        item.uri = uri;
        item.title = id_from_uri(uri);

        bool resolved = false;
        if (curl && !token.empty() && !base.empty() &&
            uri.rfind("spotify:track:", 0) == 0) {
            std::string url = base + "/metadata/4/track/" + gid_hex_from_uri(uri);
            CurlBuffer chunk{static_cast<char *>(malloc(1)), 0};
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void *>(&chunk));
            CURLcode res = curl_easy_perform(curl);
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            if (res == CURLE_OK && code == 200 && chunk.size > 0) {
                std::vector<uint8_t> data(chunk.memory, chunk.memory + chunk.size);
                Track track = Track_init_zero;
                try {
                    pbDecode(track, Track_fields, data);
                    if (track.name) {
                        item.title = std::string(track.name);
                    }
                    if (track.artist_count > 0 && track.artist[0].name) {
                        item.artist = std::string(track.artist[0].name);
                    }
                    resolved = true;
                } catch (...) {
                }
                pb_release(Track_fields, &track);
            }
            free(chunk.memory);
        }
        (void)resolved;
        out.push_back(std::move(item));
    }

    if (curl) {
        curl_easy_cleanup(curl);
    }
    curl_slist_free_all(hdrs);

    CSPOT_LOG(info, "fetch_track_results: %d items", (int)out.size());
    return static_cast<int>(out.size());
}

namespace {
// --- Minimal protobuf wire reader (just enough for the rootlist response) ---
struct PbField {
    uint32_t field;
    uint32_t wire;
    const uint8_t *data;  // length-delimited payload (wire == 2)
    size_t len;
};

bool pb_read_varint(const uint8_t *&p, const uint8_t *end, uint64_t &out) {
    out = 0;
    int shift = 0;
    while (p < end && shift < 64) {
        const uint8_t b = *p++;
        out |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            return true;
        }
        shift += 7;
    }
    return false;
}

bool pb_next(const uint8_t *&p, const uint8_t *end, PbField &f) {
    if (p >= end) {
        return false;
    }
    uint64_t tag = 0;
    if (!pb_read_varint(p, end, tag)) {
        return false;
    }
    f.field = static_cast<uint32_t>(tag >> 3);
    f.wire = static_cast<uint32_t>(tag & 0x7);
    f.data = nullptr;
    f.len = 0;
    switch (f.wire) {
        case 0: {  // varint
            uint64_t v;
            return pb_read_varint(p, end, v);
        }
        case 2: {  // length-delimited
            uint64_t len = 0;
            if (!pb_read_varint(p, end, len) ||
                static_cast<uint64_t>(end - p) < len) {
                return false;
            }
            f.data = p;
            f.len = static_cast<size_t>(len);
            p += len;
            return true;
        }
        case 5:  // 32-bit
            if (end - p < 4) {
                return false;
            }
            p += 4;
            return true;
        case 1:  // 64-bit
            if (end - p < 8) {
                return false;
            }
            p += 8;
            return true;
        default:
            return false;  // groups unsupported
    }
}

// Returns the first length-delimited string with the given field number.
std::string pb_string_field(const uint8_t *data, size_t len, uint32_t field) {
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    PbField f;
    while (pb_next(p, end, f)) {
        if (f.field == field && f.wire == 2 && f.data) {
            return std::string(reinterpret_cast<const char *>(f.data), f.len);
        }
    }
    return "";
}

// Returns the first length-delimited sub-message payload for a field number.
bool pb_message_field(const uint8_t *data, size_t len, uint32_t field,
                      const uint8_t *&out, size_t &out_len) {
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    PbField f;
    while (pb_next(p, end, f)) {
        if (f.field == field && f.wire == 2 && f.data) {
            out = f.data;
            out_len = f.len;
            return true;
        }
    }
    return false;
}
}  // namespace

int API::get_playlists(std::vector<PlaylistEntry> &out, size_t max_items) {
    std::string token = get_token();
    std::string base = get_spclient_base();
    std::string user;
    {
        std::lock_guard<std::mutex> lock(token_mutex);
        user = username;
    }
    if (token.empty() || base.empty() || user.empty()) {
        return -1;
    }

    std::string url = base;
    url += "/playlist/v2/user/";
    url += url_encode_path(user);
    url += "/rootlist?decorate=revision,attributes,length,owner,capabilities";
    url += "&from=0&length=" + std::to_string(max_items);

    Headers headers = { {"Accept: application/protobuf"},
                        {"User-Agent: Spotify/8.6.84 iOS/15.1 (iPhone11,8)"},
                        {"Authorization: Bearer " + token} };

    uint8_t *buf = nullptr;
    int len = download(url.c_str(), &buf, "GET", "", headers);
    if (len <= 0 || buf == nullptr) {
        if (buf) {
            free(buf);
        }
        CSPOT_LOG(error, "rootlist fetch failed for %s", user.c_str());
        return 0;
    }

    // SelectedListContent.contents (field 5) -> ListItems.
    const uint8_t *contents = nullptr;
    size_t contents_len = 0;
    if (!pb_message_field(buf, static_cast<size_t>(len), 5, contents,
                          contents_len)) {
        free(buf);
        CSPOT_LOG(error, "rootlist: no contents field");
        return 0;
    }

    // ListItems.items (field 3, Item.uri = 1) and meta_items (field 4,
    // MetaItem.attributes = 2 -> ListAttributes.name = 1) are index-aligned.
    std::vector<std::string> uris;
    std::vector<std::string> names;
    const uint8_t *p = contents;
    const uint8_t *end = contents + contents_len;
    PbField f;
    while (pb_next(p, end, f)) {
        if (f.field == 3 && f.wire == 2) {
            uris.push_back(pb_string_field(f.data, f.len, 1));
        } else if (f.field == 4 && f.wire == 2) {
            const uint8_t *attrs = nullptr;
            size_t attrs_len = 0;
            if (pb_message_field(f.data, f.len, 2, attrs, attrs_len)) {
                names.push_back(pb_string_field(attrs, attrs_len, 1));
            } else {
                names.push_back("");
            }
        }
    }
    free(buf);

    for (size_t i = 0; i < uris.size() && out.size() < max_items; i++) {
        if (uris[i].rfind("spotify:playlist:", 0) != 0) {
            continue;  // skip folder start/end group markers
        }
        PlaylistEntry e;
        e.uri = uris[i];
        e.name = (i < names.size() && !names[i].empty())
                     ? names[i]
                     : id_from_uri(uris[i]);
        out.push_back(std::move(e));
    }

    CSPOT_LOG(info, "rootlist -> %d playlists", (int)out.size());
    return static_cast<int>(out.size());
}

void API::play_by_uri(std::string uri, uint32_t offset_pos, uint32_t position_ms) {
    std::string token = get_token();
    if (token.size() == 0) {
        return;
    }

    std::string target_device;
    {
        std::lock_guard<std::mutex> lock(token_mutex);
        target_device = device_id.empty() ? std::string(DEVICE_ID) : device_id;
    }

    std::string url = SPOTIFY_API_PLAY_URL;
    url += "?device_id=";
    url += target_device;

    // TODO(michal4132): Replace with json object
    std::string post_data = "{\"context_uri\": \"";
    post_data += uri;
    post_data += "\",\"offset\": {\"position\": ";
    post_data += std::to_string(offset_pos);
    post_data += "},\"position_ms\": ";
    post_data += std::to_string(position_ms);
    post_data += "}";
    uint8_t *buf;
    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };
    int len = download(url.c_str(), &buf, "PUT", post_data, headers);
    if (len > 0) {
        CSPOT_LOG(info, "play_by_uri response: %.*s", len, buf);
        free(buf);
    }
}

// TODO(michal4132): limit, offset
int API::get_current_users_playlists(uint8_t **buf, uint16_t limit, uint16_t offset) {
    std::string token = get_token();
    if (token.size() == 0) {
        return -1;
    }

    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };

    std::string url = SPOTIFY_API_GET_USERS_PLAYLISTS;
    url += "?limit=";
    url += std::to_string(limit);
    url += "&offset=";
    url += std::to_string(offset);

    int len = download(url.c_str(), buf, "GET", "", headers);
    if (len <= 0) {
        buf = NULL;
        return 0;
    }

    CSPOT_LOG(info, "get_current_users_playlists response: %.*s", len, *buf);
    return len;
}

int API::get_playlist_items(uint8_t **buf, std::string playlist_id, std::string fields,
                                        uint16_t limit, uint16_t offset) {
    std::string token = get_token();
    if (token.size() == 0) {
        return -1;
    }

    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };

    std::string url = "";
    if (playlist_id.starts_with("https://api.spotify.com/v1/playlists/")) {
        url += playlist_id;

    } else {
        url += SPOTIFY_API_GET_PLAYLIST_ITEMS_s;
        url += playlist_id;
        url += SPOTIFY_API_GET_PLAYLIST_ITEMS_e;
    }
    url += "?fields=";
    url += fields;
    url += "&limit=";
    url += std::to_string(limit);
    url += "&offset=";
    url += std::to_string(offset);

    int len = download(url.c_str(), buf, "GET", "", headers);
    if (len <= 0) {
        buf = NULL;
        return 0;
    }

    CSPOT_LOG(info, "get_playlist_items response: %.*s", len, *buf);
    return len;
}

int API::get_available_devices(uint8_t **buf) {
    std::string token = get_token();
    if (token.size() == 0) {
        return -1;
    }

    Headers headers = { {"Accept: application/json"},
                        {"Content-Type: application/json"},
                        {"Authorization: Bearer " + token} };

    int len = download(SPOTIFY_API_GET_AVAILABLE_DEVICES, buf, "GET", "", headers);
    if (len <= 0) {
        buf = NULL;
        return 0;
    }

    CSPOT_LOG(info, "get_available_devices response: %.*s", len, *buf);
    return len;
}

