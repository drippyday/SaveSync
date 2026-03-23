#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist/server"
VERSION="${1:-dev}"
PKG_DIR="$DIST_DIR/gbasync-server-${VERSION}"
IMAGE_TAG="gbasync-server:${VERSION}"

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"

echo "Building Docker image: $IMAGE_TAG"
docker build -f "$ROOT_DIR/server/Dockerfile" -t "$IMAGE_TAG" "$ROOT_DIR"

echo "Saving image tarball..."
docker save "$IMAGE_TAG" -o "$PKG_DIR/gbasync-server-${VERSION}.tar"

cp "$ROOT_DIR/.env.example" "$PKG_DIR/.env.example"
cp "$PKG_DIR/.env.example" "$PKG_DIR/.env"

cp "$ROOT_DIR/server/distribution/README.md" "$PKG_DIR/README.md"

# Dist bundle has no Dockerfile — compose must use the pre-built image from the tarball.
cat > "$PKG_DIR/docker-compose.yml" <<EOF
services:
  gbasync-server:
    image: $IMAGE_TAG
    ports:
      - "8080:8080"
    env_file:
      - .env
    environment:
      SAVE_ROOT: /data/saves
      HISTORY_ROOT: /data/history
      INDEX_PATH: /data/index.json
    volumes:
      - ../../save_data:/data
      - \${HOME}/Documents/GBA:/roms:ro
    restart: unless-stopped
EOF

(
  cd "$DIST_DIR"
  rm -f "gbasync-server-${VERSION}.zip"
  zip -qr "gbasync-server-${VERSION}.zip" "gbasync-server-${VERSION}"
)

echo "Server release artifacts:"
echo "  $PKG_DIR/"
echo "  $DIST_DIR/gbasync-server-${VERSION}.zip"
