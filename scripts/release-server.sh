#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="$ROOT_DIR/server"
DIST_DIR="$ROOT_DIR/dist/server"
VERSION="${1:-dev}"
IMAGE_TAG="savesync-server:${VERSION}"

mkdir -p "$DIST_DIR"

echo "Building Docker image: $IMAGE_TAG"
docker build -t "$IMAGE_TAG" "$SERVER_DIR"

echo "Saving image tarball..."
docker save "$IMAGE_TAG" -o "$DIST_DIR/savesync-server-${VERSION}.tar"

cp "$SERVER_DIR/.env.example" "$DIST_DIR/.env.example"
# Starter env so `docker compose up` works without a manual cp (edit API_KEY before real use).
cp "$DIST_DIR/.env.example" "$DIST_DIR/.env"

# Dist bundle has no Dockerfile — compose must use the pre-built image from the tarball.
cat > "$DIST_DIR/docker-compose.yml" <<EOF
services:
  savesync-server:
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
    restart: unless-stopped
EOF

cat > "$DIST_DIR/README.txt" <<EOF
SaveSync Server Release
=======================

Image tag: $IMAGE_TAG
Tarball: savesync-server-${VERSION}.tar

Load image:
  docker load -i savesync-server-${VERSION}.tar

Run compose (no build — image is already loaded):
  docker compose up -d

(Release copies .env.example → .env for you. To reset: cp -f .env.example .env)

Develop / rebuild from source (requires repo checkout):
  cd /path/to/SaveSync/server
  docker compose build --no-cache && docker compose up -d

Host storage:
- Inside the full SaveSync repo, dist/server/docker-compose.yml mounts ../../save_data
  (repository-root save_data/, same as development from server/).
- If you copied only this folder elsewhere, edit docker-compose.yml and set the volume to
  ./save_data:/data so data stays next to this directory.
EOF

echo "Server release artifacts created in: $DIST_DIR"
