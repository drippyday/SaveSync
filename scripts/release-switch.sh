#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SWITCH_DIR="$ROOT_DIR/switch-client"
DIST_DIR="$ROOT_DIR/dist/switch"
VERSION="${1:-dev}"
OUT_DIR="$DIST_DIR/gbasync-switch-${VERSION}"

rm -rf "$OUT_DIR"

if [[ -z "${DEVKITPRO:-}" ]]; then
  if [[ -d "/opt/devkitpro" ]]; then
    export DEVKITPRO="/opt/devkitpro"
  else
    echo "DEVKITPRO is not set. Install devkitPro + libnx first."
    exit 1
  fi
fi

mkdir -p "$OUT_DIR"
mkdir -p "$OUT_DIR/gba-sync"

make -C "$SWITCH_DIR" clean
make -C "$SWITCH_DIR"

for artifact in "$SWITCH_DIR"/*.nro "$SWITCH_DIR"/*.nacp "$SWITCH_DIR"/*.elf; do
  if [[ -f "$artifact" ]]; then
    cp "$artifact" "$OUT_DIR/"
  fi
done

cp "$SWITCH_DIR/distribution/README.md" "$OUT_DIR/README.md"
cp "$SWITCH_DIR/distribution/gba-sync/README.md" "$OUT_DIR/gba-sync/README.md"
cp "$SWITCH_DIR/distribution/gba-sync/config.ini" "$OUT_DIR/gba-sync/config.ini"

(
  cd "$DIST_DIR"
  rm -f "gbasync-switch-${VERSION}.zip"
  zip -qr "gbasync-switch-${VERSION}.zip" "gbasync-switch-${VERSION}"
)

echo "Switch release artifacts:"
echo "  $OUT_DIR/"
echo "  $DIST_DIR/gbasync-switch-${VERSION}.zip"
