#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREEDS_DIR="$ROOT_DIR/3ds-client"
DIST_DIR="$ROOT_DIR/dist/3ds"
VERSION="${1:-dev}"
OUT_DIR="$DIST_DIR/gbasync-3ds-${VERSION}"

rm -rf "$OUT_DIR"

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

cp "$THREEDS_DIR/distribution/README.md" "$OUT_DIR/README.md"
cp "$THREEDS_DIR/distribution/gba-sync/README.md" "$OUT_DIR/gba-sync/README.md"
cp "$THREEDS_DIR/distribution/gba-sync/config.ini" "$OUT_DIR/gba-sync/config.ini"

(
  cd "$DIST_DIR"
  rm -f "gbasync-3ds-${VERSION}.zip"
  zip -qr "gbasync-3ds-${VERSION}.zip" "gbasync-3ds-${VERSION}"
)

echo "3DS release artifacts:"
echo "  $OUT_DIR/"
echo "  $DIST_DIR/gbasync-3ds-${VERSION}.zip"
