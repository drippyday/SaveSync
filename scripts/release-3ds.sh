#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREEDS_DIR="$ROOT_DIR/3ds-client"
DIST_DIR="$ROOT_DIR/dist/3ds"
VERSION="${1:-dev}"
OUT_DIR="$DIST_DIR/gbasync-3ds-${VERSION}"

if [[ "$VERSION" =~ ^v?([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  major="${BASH_REMATCH[1]}"
  minor="${BASH_REMATCH[2]}"
  patch="${BASH_REMATCH[3]}"
  cia_version_dec=$((10#$major * 10000 + 10#$minor * 100 + 10#$patch))
else
  cia_version_dec=$(( ( $(date +%s) / 60 ) % 65535 ))
fi
CIA_VERSION_HEX="$(printf '0x%04X' "$cia_version_dec")"

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
mkdir -p "$OUT_DIR/gba-sync"

make -C "$THREEDS_DIR" clean
make -C "$THREEDS_DIR" CIA_VERSION_HEX="$CIA_VERSION_HEX"

for artifact in "$THREEDS_DIR"/*.3dsx "$THREEDS_DIR"/*.cia "$THREEDS_DIR"/*.smdh; do
  if [[ -f "$artifact" ]]; then
    cp "$artifact" "$OUT_DIR/"
  fi
done

cat > "$OUT_DIR/INSTALL.txt" <<EOF
GBAsync 3DS Client - Install Guide
==================================

Artifacts:
- gbasync.3dsx
- gbasync.cia
- gbasync.smdh

Copy to SD:
- gbasync.3dsx -> sdmc:/3ds/gbasync.3dsx
- install gbasync.cia with FBI (optional, for HOME Menu launch)
- copy gba-sync/config.ini -> sdmc:/3ds/gba-sync/config.ini

config.ini example:

[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/3ds/open_agb_firm/saves/
# when mode=vc, app uses vc_save_dir instead of save_dir
vc_save_dir=sdmc:/3ds/Checkpoint/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba

Run:
1) Launch Homebrew Launcher
2) Start gbasync
3) A = full sync (per-game newer local mtime vs server; may upload some, download others)
4) X = upload-only: confirm, pick saves; START or R or X to run, B = cancel
5) Y = download-only: pick saves; START or R or Y to run, B = cancel
6) SELECT = trigger Dropbox sync-once on server
7) Press START to exit

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
- gbasync.cia: installable package for HOME Menu launch
- gbasync.smdh: icon and metadata displayed by launcher
EOF

cat > "$OUT_DIR/gba-sync/config.ini" <<EOF
[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/3ds/open_agb_firm/saves/
# when mode=vc, app uses vc_save_dir instead of save_dir
vc_save_dir=sdmc:/3ds/Checkpoint/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
EOF

echo "3DS release artifacts created in: $OUT_DIR"
