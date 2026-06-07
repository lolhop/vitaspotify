#!/usr/bin/env bash
# Vita VPK install requires sce_sys PNGs as 8-bit indexed (color type 3).
# 24-bit RGB icon0.png causes install error 0x8010113D in VitaShell.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ICON_DIR="$ROOT/assets/sce_sys"
SRC="$ICON_DIR/icon0_src.png"
OUT="$ICON_DIR/icon0.png"
TMP="$(mktemp /tmp/vitaspotify_icon.XXXXXX.png)"

cleanup() { rm -f "$TMP"; }
trap cleanup EXIT

if [[ -f "$SRC" ]]; then
  INPUT="$SRC"
else
  INPUT="$OUT"
fi

if [[ ! -f "$INPUT" ]]; then
  echo "No icon found at $SRC or $OUT" >&2
  exit 1
fi

for cmd in ffmpeg pngquant python3; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Missing $cmd (brew install ffmpeg pngquant)" >&2
    exit 1
  fi
done

ffmpeg -y -i "$INPUT" -vf scale=128:128:flags=lanczos -pix_fmt rgb24 "$TMP" >/dev/null 2>&1
pngquant --quality=70-95 --strip --force "$TMP" -o "$OUT"

python3 - "$OUT" <<'PY'
import struct, sys
path = sys.argv[1]
with open(path, "rb") as f:
    f.read(8)
    while True:
        ln = f.read(4)
        if not ln:
            break
        length = struct.unpack(">I", ln)[0]
        ctype = f.read(4).decode()
        data = f.read(length)
        f.read(4)
        if ctype == "IHDR":
            w, h, bit, ctype_num, *_ = struct.unpack(">IIBBBBB", data)
            if w != 128 or h != 128 or bit != 8 or ctype_num != 3:
                raise SystemExit(
                    f"icon0.png must be 128x128 8-bit indexed, got {w}x{h} bit={bit} type={ctype_num}"
                )
            print(f"icon0.png OK: {w}x{h} 8-bit indexed")
            break
        if ctype == "IEND":
            break
PY
