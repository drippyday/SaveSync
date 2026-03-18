#!/usr/bin/env bash
set -eEuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$ROOT_DIR/.tmp/smoke-sync"
SERVER_VENV="$ROOT_DIR/server/.venv/bin/activate"
BRIDGE_VENV="$ROOT_DIR/bridge/.venv/bin/activate"
SERVER_PORT="${SMOKE_SYNC_PORT:-}"
if [[ -z "$SERVER_PORT" ]]; then
  SERVER_PORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
)"
fi
SERVER_URL="http://127.0.0.1:${SERVER_PORT}"
API_KEY="${SMOKE_SYNC_API_KEY:-change-me}"
SERVER_LOG="$TMP_DIR/server.log"

if [[ ! -f "$SERVER_VENV" ]]; then
  echo "Missing server venv: $SERVER_VENV"
  exit 1
fi
if [[ ! -f "$BRIDGE_VENV" ]]; then
  echo "Missing bridge venv: $BRIDGE_VENV"
  exit 1
fi

rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR/server-data/saves" "$TMP_DIR/server-data/history" "$TMP_DIR/delta"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && ps -p "$SERVER_PID" >/dev/null 2>&1; then
    kill "$SERVER_PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT
trap 'echo "[error] smoke test failed"; [[ -f "$SERVER_LOG" ]] && echo "--- server.log ---" && cat "$SERVER_LOG"' ERR

echo "[1/7] Starting isolated server..."
(
  cd "$ROOT_DIR/server"
  source .venv/bin/activate
  SAVE_ROOT="$TMP_DIR/server-data/saves" \
  HISTORY_ROOT="$TMP_DIR/server-data/history" \
  INDEX_PATH="$TMP_DIR/server-data/index.json" \
  API_KEY="$API_KEY" \
  exec uvicorn app.main:app --host 127.0.0.1 --port "$SERVER_PORT" >"$SERVER_LOG" 2>&1
) &
SERVER_PID=$!

echo "[2/7] Waiting for server health..."
for _ in $(seq 1 30); do
  if curl -sf "$SERVER_URL/health" >/dev/null; then
    break
  fi
  sleep 1
done
curl -sf "$SERVER_URL/health" >/dev/null

echo "[3/7] Creating deterministic test save..."
python3 - <<'PY'
from pathlib import Path
p = Path(".tmp/smoke-sync/delta/Pokemon Emerald.sav")
p.write_bytes(bytes([0x42]) * 64)
print("created", p)
PY

echo "[4/7] Writing temporary bridge config..."
python3 - <<PY
import json
from pathlib import Path
cfg = {
    "server_url": "$SERVER_URL",
    "api_key": "$API_KEY",
    "delta_save_dir": str(Path(".tmp/smoke-sync/delta").resolve()),
    "poll_seconds": 5,
    "rom_dirs": [],
    "rom_map_path": None,
    "rom_extensions": [".gba"],
}
Path(".tmp/smoke-sync/config.json").write_text(json.dumps(cfg), encoding="utf-8")
print("wrote config")
PY

echo "[5/7] Running bridge one-shot upload..."
(
  cd "$ROOT_DIR/bridge"
  source .venv/bin/activate
  python bridge.py --config "$TMP_DIR/config.json" --once
)

echo "[6/7] Validating server save list..."
curl -sf -H "X-API-Key: $API_KEY" "$SERVER_URL/saves" | python3 -c '
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    raise SystemExit("empty /saves response")
data = json.loads(raw)
saves = data.get("saves", [])
assert saves, "no saves found on server"
print("server_saves:", len(saves))
'

echo "[7/7] Validating download path..."
rm -f "$TMP_DIR/delta/"*.sav
(
  cd "$ROOT_DIR/bridge"
  source .venv/bin/activate
  python bridge.py --config "$TMP_DIR/config.json" --once
)
test -f "$TMP_DIR/delta/Pokemon Emerald.sav"

echo "Smoke sync PASS"
