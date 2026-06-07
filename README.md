# VitaSpotify

Spotify Connect client for jailbroken PS Vita. Stream your library, browse playlists, search, download tracks for offline playback, and more — built on [cspot](https://github.com/feelfreelinux/cspot).

**Requires Spotify Premium.** Unofficial client — use at your own risk.

## Release install (users)

1. Install **[iTLS-Enso](https://github.com/CelesteBlue-dev/ITLS-Enso)** on your Vita.
2. Install **`vitaspotify.vpk`** on the console.
3. Unzip **`VitaSpotify-auth-helper.zip`** on your computer (Mac / Linux / Windows — no git, no coding).  
   Double-click the helper for your OS → Spotify app → Connect → **CSpot player** → copy **`auth.json`** to `ux0:data/vitaspotify/`.  
   **[Full guide: docs/AUTH.md](docs/AUTH.md)** · plain-text steps in the zip’s **README.txt**
4. Launch **VitaSpotify** → **Log in (auth.json)**.

## Vita requirements

- Jailbroken PS Vita (henkaku / similar)
- [iTLS-Enso](https://github.com/CelesteBlue-dev/ITLS-Enso) (or current TLS solution) for HTTPS
- Spotify **Premium** account

## Build (Mac or Linux / WSL)

### Apple Silicon (this machine)

VitaSDK is installed at **`~/vitasdk`** (no sudo). Your `~/.zshrc` already has:

```bash
export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
```

Open a **new terminal** (or `source ~/.zshrc`), then:

```bash
# One-time host tools (Homebrew)
brew install cmake wget protobuf@21
python3.13 -m pip install 'setuptools<70' 'protobuf>=3.19,<5'

cd /Users/sam/vitaspotify
./scripts/build.sh
```

Output: `build/vitaspotify.vpk`

### Fresh install (any Mac)

```bash
brew install cmake wget
git clone https://github.com/vitasdk/vdpm ~/vdpm
export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
# Download ARM64 toolchain (bootstrap Python SSL may fail on some Macs):
mkdir -p "$VITASDK"
curl -fL "$(curl -fsSL 'https://api.github.com/repos/vitasdk/autobuilds/releases?per_page=5' | python3 -c "
import json,sys
for r in json.load(sys.stdin):
    if 'osx' in r.get('tag_name','') and r.get('assets'):
        print(r['assets'][0]['browser_download_url']); break
")" | tar xj -C "$VITASDK" --strip-components=1
cd ~/vdpm && ./vdpm -f mbedtls zstd curl openssl freetype vitaGL imgui vitaShaRK taihen SceShaccCgExt libmathneon zlib
```

Add the `VITASDK` lines to `~/.zshrc`, then build vitaspotify as above.

See also [Vita SDK on macOS ARM](https://itspazaz.com/2026-04-26-Vita-SDK-macOS-ARM/).

## Install on Vita

1. Copy `vitaspotify.vpk` to the console (VitaShell USB/FTP).
2. Install the VPK.
3. Launch **VitaSpotify**.

## Login (`auth.json`)

End users: use **`VitaSpotify-auth-helper.zip`** from the release (see [docs/AUTH.md](docs/AUTH.md)).

Developers packing a release:

```bash
./scripts/pack-auth-helper.sh   # → build/VitaSpotify-auth-helper.zip
```

In-repo only: `./scripts/get-auth-json.sh`  
**Example file shape:** [auth.json.example](auth.json.example)

Logs on the Vita: `ux0:data/vitaspotify/log.txt`

## Controls (summary)

- **D-pad** — move highlight · **Cross** — select · **Touch** — tap controls
- **L / R** — page lists (downloads, search results, playlists)
- **Go offline** — disconnect Spotify and play downloaded tracks
- **START + SELECT** — quit cleanly (preferred over swiping the LiveArea bubble closed)

## Can't delete the bubble / uninstall error

If LiveArea won't remove VitaSpotify (often after a crash or force-close), the app is probably still running or left in a bad state.

1. **Force-stop it** — Hold **PS** → highlight VitaSpotify → **Close** (or restart the Vita).
2. **Delete with VitaShell** (most reliable):
   - Open **VitaShell** → `ux0:app/`
   - Find folder **`VSPOT0001`**
   - **Triangle** → **Delete**
   - If you installed to a memory card, also check `imc0:app/VSPOT0001`
3. **Refresh LiveArea** — Reboot the Vita. The bubble should be gone.
4. **Optional cleanup** (does not remove the bubble by itself):
   - `ux0:data/vitaspotify/` — saved login and logs

Then install the latest `vitaspotify.vpk` again. Newer builds use **START+SELECT** to quit cleanly, which helps avoid this.

If deletion still fails, note the **exact error text** (or error code like `C2-…`) and whether the folder exists in `ux0:app/`.

## Project layout

| Path | Purpose |
|------|---------|
| `src/` | Vita app (text UI, audio, API) |
| `third_party/cspot/` | Spotify Connect core (git submodule) |
| `patch/Queue.h` | Vita-safe bell queue (applied at configure time) |
| `assets/` | Icon, UI artwork |
| `docs/AUTH.md` | User guide for creating `auth.json` |
| `scripts/get-auth-json.sh` | One-command `auth.json` generator (Mac/Linux) |

## Roadmap

- [x] `auth.json` login via PC helper script
- [x] Liked Songs / playlists / search
- [x] Offline downloads
- [ ] CJK font support for track titles

## License

App code: MIT (see LICENSE). `third_party/cspot` follows its own license. Not affiliated with Spotify.
