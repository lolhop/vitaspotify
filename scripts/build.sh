#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ -z "${VITASDK:-}" ]]; then
  export VITASDK="${HOME}/vitasdk"
fi
if [[ ! -d "$VITASDK/bin" ]]; then
  echo "VITASDK not found at $VITASDK. Install VitaSDK first (see README.md)." >&2
  exit 1
fi

export PATH="/opt/homebrew/opt/protobuf@21/bin:/opt/homebrew/bin:$VITASDK/bin:$PATH"
export PROTOC="${PROTOC:-/opt/homebrew/opt/protobuf@21/bin/protoc}"

git submodule update --init --recursive

"$ROOT/scripts/optimize_icon0.sh"

mkdir -p build
cd build
cmake ..
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "Built: $ROOT/build/vitaspotify.vpk"
