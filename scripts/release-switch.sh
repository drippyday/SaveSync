#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SWITCH_DIR="$ROOT_DIR/switch-client"
DIST_DIR="$ROOT_DIR/dist/switch"
VERSION="${1:-dev}"
OUT_DIR="$DIST_DIR/savesync-switch-${VERSION}"

if [[ -z "${DEVKITPRO:-}" ]]; then
  if [[ -d "/opt/devkitpro" ]]; then
    export DEVKITPRO="/opt/devkitpro"
  else
    echo "DEVKITPRO is not set. Install devkitPro + libnx first."
    exit 1
  fi
fi

mkdir -p "$OUT_DIR"

make -C "$SWITCH_DIR" clean
make -C "$SWITCH_DIR"

for artifact in "$SWITCH_DIR"/*.nro "$SWITCH_DIR"/*.nacp "$SWITCH_DIR"/*.elf; do
  if [[ -f "$artifact" ]]; then
    cp "$artifact" "$OUT_DIR/"
  fi
done

cat > "$OUT_DIR/INSTALL.txt" <<EOF
SaveSync Switch Client - Install Guide
======================================

Artifact:
- gbasync.nro
- gbasync.nacp
- gbasync.elf

Copy to SD:
- gbasync.nro -> sdmc:/switch/gbasync.nro
- create sdmc:/switch/gba-sync/config.ini

config.ini example:

[server]
url=http://192.168.1.50:8080
api_key=change-me

[sync]
save_dir=sdmc:/roms/gba/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba

Run:
1) Launch Homebrew Menu
2) Start gbasync
3) A = full sync (per-game newer local mtime vs server; may upload some, download others)
4) X = upload-only: confirm, then checkboxes (ALL SAVES or each game), + = OK, B = cancel
5) Y = download-only: same pick list, then downloads selected games
6) Press + to exit

Expected status example:
- Local saves: 3
- Remote saves: 2
- pokemon-emerald: UPLOADED
- metroid-zero: DOWNLOADED

Troubleshooting:
- config path must be sdmc:/switch/gba-sync/config.ini
- use http:// URL for this MVP
- ensure API key matches server

What each artifact is:
- gbasync.nro: app you run from Homebrew Menu
- gbasync.nacp: metadata file (title/author/version)
- gbasync.elf: raw executable/debug artifact (not usually copied to SD)
EOF

echo "Switch release artifacts created in: $OUT_DIR"
