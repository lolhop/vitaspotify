#pragma once

class App;

// Bundled assets from the VPK live under app0:/ at runtime.
#define APP0_PREFIX           "app0:/"
#define UI_ASSETS_PREFIX      APP0_PREFIX "assets/ui/"

#define DEVICE_NAME           "PS Vita (VitaSpotify)"
#define CREDENTIALS_FILE_NAME "ux0:data/vitaspotify/auth.json"
// Local-only test creds (create on Vita; never commit — see dev_login.example.txt)
#define DEV_LOGIN_FILE_NAME   "ux0:data/vitaspotify/dev_login.txt"
#define CONFIG_FILE_NAME      "ux0:data/vitaspotify/config.json"
#define LOG_FILE_NAME         "ux0:data/vitaspotify/log.txt"
#define OFFLINE_DL_DIR        "ux0:data/vitaspotify/dl/"

#define CLIENT_ID_ANDROID     "65b708073fc0480ea92a077233ca87bd"
#define DEVICE_ID             "142137fd329622137a14901634264e6f332e2411"
#define SCOPES                "user-read-playback-state,user-modify-playback-state,playlist-read-private,playlist-read-collaborative"  // NOLINT
#define USER_AGENT            "Spotify/8.6.84 iOS/15.1 (iPhone11,8)"

// Stops the background Spotify network thread (safe to call more than once).
void vitaspotify_stop_network_thread();
// Stops online decode + PCM without tearing down the shared audio port.
void vitaspotify_stop_spotify_pipeline();
// Pause speakers, stop CDN decode, then stop PCM output (safe during mode switch).
void vitaspotify_halt_online_playback();
// Releases BGM + audio ports immediately (idempotent; safe from any thread).
void vitaspotify_release_all_audio_resources();
// Poll LiveArea / AppMgr quit events (call every frame from the main thread).
bool vitaspotify_poll_livearea_quit(App *app);
// LiveArea force-close: release resources and exit the process immediately.
void vitaspotify_terminate_process(App *app);
// Cancel the cspot kernel thread without blocking on teardown.
void vitaspotify_cancel_network_thread();
