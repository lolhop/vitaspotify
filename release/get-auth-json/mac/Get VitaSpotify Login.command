#!/bin/bash
# VitaSpotify auth.json helper — Mac (double-click this file)
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
HELPER="$DIR/vitaspotify-auth-helper"
OUT="$HOME/Desktop/vitaspotify auth.json"

clear
echo ""
echo "=============================================="
echo "  VitaSpotify — create auth.json"
echo "=============================================="
echo ""

if [[ ! -x "$HELPER" ]]; then
  echo "ERROR: vitaspotify-auth-helper is missing from this folder."
  echo ""
  echo "The zip should include both:"
  echo "  mac/vitaspotify-auth-helper"
  echo "  mac/Get VitaSpotify Login.command"
  echo ""
  read -r -p "Press Enter to close..."
  exit 1
fi

echo "This will create a login file on your Desktop:"
echo "  $OUT"
echo ""
echo "STEPS:"
echo "  1. Leave this window open."
echo "  2. Open Spotify on your phone (same Wi-Fi as this Mac)."
echo "  3. Play any song → tap Connect to a device (speaker icon)."
echo "  4. Choose:  CSpot player"
echo "  5. Wait until you see SUCCESS below (may take 10–30 seconds)."
echo ""
echo "Starting helper..."
echo ""

if xattr -l "$HELPER" 2>/dev/null | grep -q com.apple.quarantine; then
  echo "(Removing macOS quarantine flag...)"
  xattr -d com.apple.quarantine "$HELPER" 2>/dev/null || true
fi

set +e
"$HELPER" -c "$OUT"
STATUS=$?
set -e

echo ""
if [[ -s "$OUT" ]]; then
  echo "=============================================="
  echo "  SUCCESS"
  echo "=============================================="
  echo ""
  echo "Created: $OUT"
  echo ""
  echo "NEXT — copy to your PS Vita:"
  echo "  1. VitaShell → enable USB or FTP"
  echo "  2. Put the file at: ux0:data/vitaspotify/auth.json"
  echo "  3. VitaSpotify → Log in (auth.json)"
  echo ""
  open -R "$OUT" 2>/dev/null || true
else
  echo "=============================================="
  echo "  NOT CREATED YET"
  echo "=============================================="
  echo ""
  echo "auth.json was not created."
  echo "Did you pick CSpot player in the Spotify app?"
  echo "Run this script again and complete the Connect step."
  echo ""
fi

read -r -p "Press Enter to close..."
exit "$STATUS"
