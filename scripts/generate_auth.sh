#!/usr/bin/env bash
# Create auth.json for VitaSpotify using Spotify's official app login (zeroconf).
# Run on Mac or Linux on the same Wi-Fi as your Vita, then copy the file to the console.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HOME/vitaspotify_auth.json}"
CLI_BUILD="$ROOT/build-host/cspotcli"

QUEUE_H="$ROOT/third_party/cspot/cspot/bell/main/utilities/include/Queue.h"
TCPSOCKET_H="$ROOT/third_party/cspot/cspot/bell/main/io/include/TCPSocket.h"
BELL_DIR="$ROOT/third_party/cspot/cspot/bell"

prefer_python_with_protobuf() {
  local candidate path dir
  for candidate in python3 /Library/Frameworks/Python.framework/Versions/3.13/bin/python3 /usr/local/bin/python3 /opt/homebrew/bin/python3 /usr/bin/python3; do
    path="$(command -v "$candidate" 2>/dev/null || true)"
    if [[ -n "$path" ]] && "$path" -c 'import google.protobuf' >/dev/null 2>&1; then
      dir="$(dirname "$path")"
      export PATH="$dir:$PATH"
      return 0
    fi
  done
  echo "Python protobuf package not found. Install it with: python3 -m pip install 'protobuf>=3.19,<5'" >&2
  exit 1
}

restore_host_cspot_headers() {
  git -C "$BELL_DIR" show HEAD:main/utilities/include/Queue.h >"$QUEUE_H"
  git -C "$BELL_DIR" show HEAD:main/io/include/TCPSocket.h >"$TCPSOCKET_H"
}

restore_vita_cspot_headers() {
  cp "$ROOT/patch/Queue.h" "$QUEUE_H"
  cp "$ROOT/patch/TCPSocket.h" "$TCPSOCKET_H"
}

ensure_host_deps() {
  if [[ "$(uname -s)" == "Darwin" ]]; then
    if ! command -v brew >/dev/null 2>&1; then
      echo "Homebrew is required on macOS (https://brew.sh)." >&2
      exit 1
    fi
    brew list portaudio >/dev/null 2>&1 || brew install portaudio
    brew list cmake >/dev/null 2>&1 || brew install cmake
    if [[ -d "/opt/homebrew/opt/protobuf@21/bin" ]]; then
      export PATH="/opt/homebrew/opt/protobuf@21/bin:${PATH:-}"
    fi
    if [[ -d "/opt/homebrew/bin" ]]; then
      export PATH="/opt/homebrew/bin:${PATH:-}"
    fi
    if [[ -z "${PROTOC:-}" && -x "/opt/homebrew/opt/protobuf@21/bin/protoc" ]]; then
      export PROTOC="/opt/homebrew/opt/protobuf@21/bin/protoc"
    fi
  else
    for cmd in cmake make protoc pkg-config; do
      if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing $cmd. Install build tools + libportaudio + protobuf (see docs/AUTH.md)." >&2
        exit 1
      fi
    done
    if ! pkg-config --exists portaudio-2.0 2>/dev/null; then
      echo "libportaudio is required (e.g. sudo apt install portaudio19-dev)." >&2
      exit 1
    fi
  fi
  prefer_python_with_protobuf
}

build_cspotcli() {
  echo "Building cspotcli for your computer (one-time)..."
  restore_host_cspot_headers
  mkdir -p "$ROOT/build-host"
  cd "$ROOT/build-host"
  cmake "$ROOT/third_party/cspot/targets/cli" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2" -DCMAKE_EXE_LINKER_FLAGS=""
  make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)" cspotcli
  restore_vita_cspot_headers
}

ensure_host_deps

if [[ ! -x "$CLI_BUILD" ]]; then
  build_cspotcli
fi

echo ""
echo "=== VitaSpotify — create auth.json ==="
echo ""
echo "1. Keep this terminal open. A helper named \"CSpot player\" will appear on your network."
echo "2. On your phone or computer, open the Spotify app (same Wi-Fi)."
echo "3. Play any song → tap the **Connect to a device** icon (speaker/TV at bottom)."
echo "4. Choose **CSpot player** and complete login if Spotify asks."
echo "5. When the terminal shows activity / \"Creating player\", press Ctrl+C."
echo "6. Copy this file to your Vita:"
echo "     $OUT"
echo "     → ux0:data/vitaspotify/auth.json"
echo ""
echo "auth.json is secret — do not share it or commit it to git."
echo ""

restore_host_cspot_headers
trap restore_vita_cspot_headers EXIT

"$CLI_BUILD" -c "$OUT" || true

if [[ -s "$OUT" ]]; then
  echo ""
  echo "Success: wrote $OUT ($(wc -c <"$OUT" | tr -d ' ') bytes)"
  echo "Next: VitaShell → ux0:data/vitaspotify/ → paste auth.json → VitaSpotify → Log in (auth.json)"
else
  echo ""
  echo "No auth.json was created." >&2
  echo "Complete the Spotify Connect step (pick CSpot player) and run this script again." >&2
  exit 1
fi
