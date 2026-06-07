#!/usr/bin/env bash
# Developers with the git repo only. End users: VitaSpotify-auth-helper.zip
# (run ./scripts/pack-auth-helper.sh) — no git or compile step required.
exec "$(cd "$(dirname "$0")" && pwd)/generate_auth.sh" "$@"
