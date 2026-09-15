# VitaSpotify

VitaSpotify is a Spotify Connect client for the PlayStation Vita. It lets a
jailbroken Vita show up as a Spotify device, play music, browse playlists and
search results, and save tracks for offline playback.

This is unofficial homebrew and requires Spotify Premium. It is built on
[cspot](https://github.com/feelfreelinux/cspot).

## What Works

- Login with an `auth.json` file generated on your computer
- Now Playing controls
- Liked Songs, playlists, search, and direct Spotify links
- Track and playlist downloads for offline playback
- Local playback for downloaded tracks without signing in again

## Install

1. Install a current TLS setup on the Vita, such as
   [iTLS-Enso](https://github.com/CelesteBlue-dev/ITLS-Enso).
2. Install `vitaspotify.vpk` with VitaShell.
3. On your computer, unzip `VitaSpotify-auth-helper.zip`.
4. Run the helper for your OS:
   - macOS: `mac/Get VitaSpotify Login.command`
   - Windows: `windows/Get VitaSpotify Login.bat`
   - Linux: `linux/get-vitaspotify-login.sh`
5. In the Spotify app, play anything, open Connect to a device, and choose
   `CSpot player`.
6. Copy the generated login file to the Vita as:

```text
ux0:data/vitaspotify/auth.json
```

Launch VitaSpotify and choose `Log in (auth.json)`.

Keep `auth.json` private. It is a saved Spotify login file, not a config file to
share in issues, screenshots, or chat logs.

The longer auth guide is in [docs/AUTH.md](docs/AUTH.md).

## Controls

- D-pad: move through lists and controls
- Cross: select
- Touch: tap playback controls
- L/R: page through downloads, playlists, and search results
- START + SELECT: quit cleanly

Use the in-app `Go offline` option to disconnect Spotify and play saved tracks.

## Build

You need VitaSDK and the Vita packages used by the app:

```bash
vdpm -f mbedtls zstd curl openssl freetype vitaGL imgui vitaShaRK taihen SceShaccCgExt libmathneon zlib
```

On macOS, install the host tools:

```bash
brew install cmake wget protobuf@21
python3 -m pip install 'setuptools<70' 'protobuf>=3.19,<5'
```

On Debian/Ubuntu or WSL:

```bash
sudo apt install build-essential cmake wget protobuf-compiler python3-protobuf
```

Then build:

```bash
git clone https://github.com/lolhop/vitaspotify.git
cd vitaspotify
export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
./scripts/build.sh
```

The VPK is written to:

```text
build/vitaspotify.vpk
```

## Auth Helper Builds

The release helper zip is made from `release/get-auth-json/`:

```bash
./scripts/pack-auth-helper.sh
```

That produces:

```text
build/VitaSpotify-auth-helper.zip
```

Build the Windows and Linux helper binaries on those platforms and copy them into
their release folders before packing a public release. The Windows helper build
uses the vendored `tools/protoc/` compiler files. See
[docs/BUILD-AUTH-HELPER.md](docs/BUILD-AUTH-HELPER.md).

## Project Layout

```text
assets/                  Vita icon and UI images
docs/                    user and release notes
include/                 app headers
patch/                   Vita/host patches applied to cspot/bell while building
release/get-auth-json/   scripts shipped with the auth helper zip
scripts/                 build and packaging scripts
src/                     Vita app code
third_party/cspot/       vendored Spotify Connect implementation
third_party/debugscreen/ debug screen helper used by the app
tools/protoc/            Windows protoc binary and protobuf include files
```

## Troubleshooting

If the LiveArea bubble will not delete after a crash, close VitaSpotify from the
PS button menu or reboot the Vita, then delete `ux0:app/VSPOT0001` with
VitaShell. Saved login and log data lives in `ux0:data/vitaspotify/`.

If login fails, check the file path first. It must be exactly:

```text
ux0:data/vitaspotify/auth.json
```

If `CSpot player` never appears in Spotify, make sure the computer running the
helper and the phone or desktop Spotify app are on the same Wi-Fi, then disable
VPNs or firewall rules that block local device discovery.

## Open Source Notes

No real Spotify credentials are checked into this repo. `auth.json` and
`dev_login.txt` are ignored on purpose.

The app code in this repository is MIT licensed. VitaSpotify also vendors cspot,
which is GPLv3. If you distribute builds that include cspot, make sure you follow
the GPLv3 requirements for that combined binary.

VitaSpotify is not affiliated with Spotify.

AI tools were used while writing and debugging the code. No generative AI was used for assets.
