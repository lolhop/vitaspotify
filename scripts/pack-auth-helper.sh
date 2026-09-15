#!/usr/bin/env bash
# Zip the auth.json helper folder for distribution alongside the VPK.
# Each OS folder must include its helper binary IN the zip (no separate download).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RELEASE_DIR="$ROOT/release/get-auth-json"
OUT_ZIP="$ROOT/build/VitaSpotify-auth-helper.zip"
OS="$(uname -s)"

build_mac_helper() {
  local dest="$RELEASE_DIR/mac/vitaspotify-auth-helper"
  local cli_build="$ROOT/build-host/cspotcli"
  local queue_h="$ROOT/third_party/cspot/cspot/bell/main/utilities/include/Queue.h"
  local tcpsocket_h="$ROOT/third_party/cspot/cspot/bell/main/io/include/TCPSocket.h"
  local bell_dir="$ROOT/third_party/cspot/cspot/bell"

  echo "Building Mac auth helper..."
  git -C "$bell_dir" show HEAD:main/utilities/include/Queue.h >"$queue_h"
  git -C "$bell_dir" show HEAD:main/io/include/TCPSocket.h >"$tcpsocket_h"
  mkdir -p "$ROOT/build-host"
  (
    cd "$ROOT/build-host"
    cmake "$ROOT/third_party/cspot/targets/cli" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_CXX_FLAGS_RELEASE="-O2" -DCMAKE_EXE_LINKER_FLAGS=""
    make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" cspotcli
  )
  cp "$ROOT/patch/Queue.h" "$queue_h"
  cp "$ROOT/patch/TCPSocket.h" "$tcpsocket_h"
  cp "$cli_build" "$dest"
  chmod +x "$dest"
  echo "  -> $dest ($(file -b "$dest"))"
}

ensure_mac_helper() {
  local dest="$RELEASE_DIR/mac/vitaspotify-auth-helper"
  if [[ -x "$dest" ]]; then
    return 0
  fi
  if [[ "$OS" != "Darwin" ]]; then
    return 0
  fi
  if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake required to build Mac helper (brew install cmake portaudio protobuf@21)" >&2
    exit 1
  fi
  export PATH="/opt/homebrew/opt/protobuf@21/bin:/opt/homebrew/bin:${PATH:-}"
  build_mac_helper
}

check_bundled_helpers() {
  local missing=0
  if [[ ! -x "$RELEASE_DIR/mac/vitaspotify-auth-helper" ]]; then
    echo "WARN: missing mac/vitaspotify-auth-helper (build on Mac or copy in)"
    missing=1
  fi
  if [[ ! -x "$RELEASE_DIR/linux/vitaspotify-auth-helper" ]]; then
    echo "WARN: missing linux/vitaspotify-auth-helper (build on Linux and copy in)"
    missing=1
  fi
  if [[ ! -f "$RELEASE_DIR/windows/vitaspotify-auth-helper.exe" ]]; then
    echo "WARN: missing windows/vitaspotify-auth-helper.exe (build on Windows and copy in)"
    missing=1
  fi
  return "$missing"
}

chmod +x "$RELEASE_DIR/mac/Get VitaSpotify Login.command" 2>/dev/null || true
chmod +x "$RELEASE_DIR/linux/get-vitaspotify-login.sh"

ensure_mac_helper

mkdir -p "$ROOT/build"
rm -f "$OUT_ZIP"
(
  cd "$RELEASE_DIR/.."
  zip -r "$OUT_ZIP" get-auth-json \
    -x "get-auth-json/.gitignore" \
    -x "get-auth-json/*/.DS_Store" \
    -x "get-auth-json/*/outputFifo" \
    -x "get-auth-json/mac/.helper-build-id"
)

echo ""
echo "Packed: $OUT_ZIP"
echo ""
if check_bundled_helpers; then
  echo ""
  echo "Zip is complete for all platforms."
else
  echo ""
  echo "Zip is missing some helpers. Before sharing widely:"
  echo "  • Mac:    ./scripts/pack-auth-helper.sh (on a Mac)"
  echo "  • Linux:  build cspotcli, copy to release/get-auth-json/linux/vitaspotify-auth-helper"
  echo "  • Windows: build cspotcli.exe, copy to release/get-auth-json/windows/vitaspotify-auth-helper.exe"
  echo "  See docs/BUILD-AUTH-HELPER.md"
fi
echo ""
echo "Share with users (one zip, everything inside):"
echo "  • build/vitaspotify.vpk"
echo "  • build/VitaSpotify-auth-helper.zip"
