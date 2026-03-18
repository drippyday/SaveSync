#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="$ROOT_DIR/server"
DIST_DIR="$ROOT_DIR/dist/server"
VERSION="${1:-dev}"
IMAGE_TAG="savesync-server:${VERSION}"

mkdir -p "$DIST_DIR"
rm -f "$DIST_DIR/.env"

echo "Building Docker image: $IMAGE_TAG"
docker build -t "$IMAGE_TAG" "$SERVER_DIR"

echo "Saving image tarball..."
docker save "$IMAGE_TAG" -o "$DIST_DIR/savesync-server-${VERSION}.tar"

cp "$SERVER_DIR/.env.example" "$DIST_DIR/.env.example"
cp "$SERVER_DIR/docker-compose.yml" "$DIST_DIR/docker-compose.yml"

cat > "$DIST_DIR/README.txt" <<EOF
SaveSync Server Release
=======================

Image tag: $IMAGE_TAG
Tarball: savesync-server-${VERSION}.tar

Load image:
  docker load -i savesync-server-${VERSION}.tar

Run compose:
  cp .env.example .env
  docker compose up -d
EOF

echo "Server release artifacts created in: $DIST_DIR"
