#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

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

if [[ -z "${VITASDK:-}" ]]; then
  export VITASDK="${HOME}/vitasdk"
fi
if [[ ! -d "$VITASDK/bin" ]]; then
  echo "VITASDK not found at $VITASDK. Install VitaSDK first (see README.md)." >&2
  exit 1
fi

if [[ -d "/opt/homebrew/opt/protobuf@21/bin" ]]; then
  export PATH="/opt/homebrew/opt/protobuf@21/bin:$PATH"
fi
if [[ -d "/opt/homebrew/bin" ]]; then
  export PATH="/opt/homebrew/bin:$PATH"
fi
export PATH="$VITASDK/bin:$PATH"
if [[ -z "${PROTOC:-}" && -x "/opt/homebrew/opt/protobuf@21/bin/protoc" ]]; then
  export PROTOC="/opt/homebrew/opt/protobuf@21/bin/protoc"
fi
prefer_python_with_protobuf

git submodule update --init --recursive

"$ROOT/scripts/optimize_icon0.sh"

mkdir -p build
cd build
cmake ..
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "Built: $ROOT/build/vitaspotify.vpk"
