#pragma once

#include <mutex>
#include <string>
#include <vector>

#define SPOTIFY_PLAYLIST_HEADER            "spotify:playlist:"
#define SPOTIFY_TRACK_FETCH_CHUNK_SIZE     50
#define SPOTIFY_PLAYLIST_FETCH_CHUNK_SIZE  15
#define SPOTIFY_PLAYLIST_FIELDS            "items(track(name,artists(name))),next"

#define SPOTIFY_API_PLAY_URL               "https://api.spotify.com/v1/me/player/play"
#define SPOTIFY_API_GET_USERS_PLAYLISTS    "https://api.spotify.com/v1/me/playlists"
#define SPOTIFY_API_GET_PLAYLIST_ITEMS_s   "https://api.spotify.com/v1/playlists/"
#define SPOTIFY_API_GET_PLAYLIST_ITEMS_e   "/tracks"
#define SPOTIFY_API_GET_AVAILABLE_DEVICES  "https://api.spotify.com/v1/me/player/devices"

struct TrackResult {
    std::string uri;
    std::string title;
    std::string artist;
};

struct PlaylistEntry {
    std::string uri;
    std::string name;
};

class API {
 public:
    API() {}
    void set_token(std::string _token);
    bool has_token();
    void set_device_id(std::string _device_id);
    void set_username(std::string _username);
    // Returns the context URI for the user's Liked Songs, or empty if unknown.
    std::string liked_songs_uri();

    // Full spclient base URL, e.g. "https://gae2-spclient.spotify.com:443".
    // The spclient backend is not subject to the public Web API (api.spotify.com)
    // 429 rate limits, so we use it to resolve contexts into track lists.
    void set_spclient_base(std::string base);
    bool has_spclient_base();

    // Resolves a Spotify context URI (playlist/album/artist/track/search) into a
    // flat list of track URIs via spclient /context-resolve. Returns the number
    // of track URIs found (>= 0), or -1 if prerequisites (token/base) are missing.
    int resolve_context_tracks(const std::string &context_uri,
                               std::vector<std::string> &out_uris,
                               size_t max_tracks = 200);

    // Fetches display metadata (title/artist) for the given track URIs over a
    // single reused HTTPS connection. URIs that fail to resolve fall back to
    // their base62 id. Returns the number of items populated.
    int fetch_track_results(const std::vector<std::string> &uris,
                            std::vector<TrackResult> &out, size_t max_items = 40);

    // Fetches the signed-in user's saved playlists (URI + display name) from the
    // spclient rootlist endpoint, which is not subject to Web API 429 limits.
    // Returns the number of playlists found, or -1 if prerequisites are missing.
    int get_playlists(std::vector<PlaylistEntry> &out, size_t max_items = 200);

    void play_by_uri(std::string uri, uint32_t offset_pos, uint32_t position_ms);
    int get_current_users_playlists(uint8_t **buf, uint16_t limit, uint16_t offset);
    int get_playlist_items(uint8_t **buf, std::string playlist_id, std::string fields, uint16_t limit, uint16_t offset);
    int get_available_devices(uint8_t **buf);

 private:
    std::string get_token();
    std::string get_spclient_base();

    std::mutex token_mutex;
    std::string token;
    std::string device_id;
    std::string spclient_base;
    std::string username;
};
