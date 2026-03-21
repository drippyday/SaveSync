#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_DIR="$ROOT_DIR/bridge"
DIST_DIR="$ROOT_DIR/dist/bridge"
VERSION="${1:-dev}"
PKG_DIR="$DIST_DIR/gbasync-bridge-${VERSION}"

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"

cp "$BRIDGE_DIR/bridge.py" "$PKG_DIR/"
cp "$BRIDGE_DIR/game_id.py" "$PKG_DIR/"
cp "$BRIDGE_DIR/dropbox_bridge.py" "$PKG_DIR/"
cp "$BRIDGE_DIR/dropbox_env.py" "$PKG_DIR/"
cp "$ROOT_DIR/.env.example" "$PKG_DIR/.env.example"
cp "$BRIDGE_DIR/DROPBOX_SETUP.md" "$PKG_DIR/"
cp "$BRIDGE_DIR/requirements.txt" "$PKG_DIR/"
cp "$BRIDGE_DIR/requirements-dropbox.txt" "$PKG_DIR/"
cp "$BRIDGE_DIR/config.example.json" "$PKG_DIR/"
cp "$BRIDGE_DIR/config.example.dropbox.json" "$PKG_DIR/"
cp "$BRIDGE_DIR/README.md" "$PKG_DIR/"
cp "$BRIDGE_DIR/DROPBOX.md" "$PKG_DIR/"
cp "$BRIDGE_DIR/DELTA_DROPBOX_FORMAT.md" "$PKG_DIR/"
cp "$BRIDGE_DIR/delta_dropbox_sav.py" "$PKG_DIR/"
cp "$BRIDGE_DIR/delta_folder_server_sync.py" "$PKG_DIR/"
cp "$BRIDGE_DIR/delta_dropbox_api_sync.py" "$PKG_DIR/"
cp "$BRIDGE_DIR/config.example.delta_sync.json" "$PKG_DIR/"
cp "$BRIDGE_DIR/config.example.delta_dropbox_api.json" "$PKG_DIR/"

cat > "$PKG_DIR/run-once.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python bridge.py --config config.example.json --once
EOF

cat > "$PKG_DIR/run-watch.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python bridge.py --config config.example.json --watch
EOF

chmod +x "$PKG_DIR/run-once.sh" "$PKG_DIR/run-watch.sh"

(
  cd "$DIST_DIR"
  zip -qr "gbasync-bridge-${VERSION}.zip" "gbasync-bridge-${VERSION}"
)

echo "Bridge release package created:"
echo "  $DIST_DIR/gbasync-bridge-${VERSION}.zip"
