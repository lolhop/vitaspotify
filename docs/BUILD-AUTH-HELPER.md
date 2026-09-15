# Building auth helpers (maintainers)

End users get **one zip** with scripts **and** helper programs in each folder. No GitHub upload step — just put the `.exe` next to the `.bat` before you zip.

## Folder layout (what goes in the zip)

```
get-auth-json/
  README.txt
  mac/
    Get VitaSpotify Login.command
    vitaspotify-auth-helper          ← include this
  linux/
    get-vitaspotify-login.sh
    vitaspotify-auth-helper          ← include this
  windows/
    Get VitaSpotify Login.bat
    vitaspotify-auth-helper.exe      ← include this
```

## Pack the zip

```bash
# Mac helper is built automatically if missing:
./scripts/pack-auth-helper.sh
```

Output: `build/VitaSpotify-auth-helper.zip`

Copy Windows/Linux helpers into the folders above first, then re-run pack if you added them.

---

## Mac (Apple Silicon)

Handled by `./scripts/pack-auth-helper.sh` on a Mac. Install the host tools
first:

```bash
brew install cmake portaudio protobuf@21
python3 -m pip install 'protobuf>=3.19,<5'
```

---

## Linux

```bash
sudo apt install build-essential cmake libportaudio2 libportaudio-dev protobuf-compiler python3-protobuf
cd vitaspotify
git submodule update --init third_party/cspot
mkdir -p build-host && cd build-host
cmake ../third_party/cspot/targets/cli -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) cspotcli
cp cspotcli ../release/get-auth-json/linux/vitaspotify-auth-helper
chmod +x ../release/get-auth-json/linux/vitaspotify-auth-helper
```

---

## Windows

Build on a Windows PC with Visual Studio + CMake + vcpkg (portaudio, protobuf). Then:

```powershell
copy build-host\Release\cspotcli.exe release\get-auth-json\windows\vitaspotify-auth-helper.exe
```

See [cspot CLI build notes](https://github.com/feelfreelinux/cspot/tree/master/targets/cli) if cmake fails.

---

## Share with users

Upload or send:

- `vitaspotify.vpk`
- `VitaSpotify-auth-helper.zip` (complete zip with all three helpers)

Users unzip and double-click the script for their OS. No git, no extra downloads.
