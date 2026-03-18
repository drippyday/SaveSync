#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREEDS_DIR="$ROOT_DIR/3ds-client"
DIST_DIR="$ROOT_DIR/dist/3ds"
VERSION="${1:-dev}"
OUT_DIR="$DIST_DIR/savesync-3ds-${VERSION}"

if [[ -z "${DEVKITPRO:-}" ]]; then
  if [[ -d "/opt/devkitpro" ]]; then
    export DEVKITPRO="/opt/devkitpro"
  else
    echo "DEVKITPRO is not set. Install devkitARM + libctru first."
    exit 1
  fi
fi

if [[ -z "${DEVKITARM:-}" ]]; then
  if [[ -d "$DEVKITPRO/devkitARM" ]]; then
    export DEVKITARM="$DEVKITPRO/devkitARM"
  else
    echo "DEVKITARM is not set. Install devkitARM + libctru first."
    exit 1
  fi
fi

mkdir -p "$OUT_DIR"

make -C "$THREEDS_DIR" clean
make -C "$THREEDS_DIR"

for artifact in "$THREEDS_DIR"/*.3dsx "$THREEDS_DIR"/*.cia "$THREEDS_DIR"/*.smdh; do
  if [[ -f "$artifact" ]]; then
    cp "$artifact" "$OUT_DIR/"
  fi
done

cat > "$OUT_DIR/INSTALL.txt" <<EOF
SaveSync 3DS Client - Install Guide
===================================

Artifacts:
- gbasync.3dsx
- gbasync.smdh

Copy to SD:
- gbasync.3dsx -> sdmc:/3ds/gbasync.3dsx
- create sdmc:/3ds/gba-sync/config.ini

config.ini example:

[server]
url=http://192.168.1.50:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/saves
# when mode=vc, app uses vc_save_dir instead of save_dir
vc_save_dir=sdmc:/3ds/Checkpoint/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba

Run:
1) Launch Homebrew Launcher
2) Start gbasync
3) A = full sync (upload-all then download-all)
4) X = upload-only (force overwrite to server)
5) Y = download-only (overwrite local from server)
6) Press START to exit

Expected status example:
- Scanning local saves...
- Local saves: 2
- Remote saves: 3
- pokemon-emerald: DOWNLOADED

Troubleshooting:
- config path must be sdmc:/3ds/gba-sync/config.ini
- use http:// URL for this MVP
- ensure API key and server IP are correct

What each artifact is:
- gbasync.3dsx: app you run from Homebrew Launcher
- gbasync.smdh: icon and metadata displayed by launcher
EOF

echo "3DS release artifacts created in: $OUT_DIR"
