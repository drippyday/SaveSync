#!/bin/sh
set -e
DROPBOX_MODE="${GBASYNC_DROPBOX_MODE:-${SAVESYNC_DROPBOX_MODE:-off}}"
if [ "$DROPBOX_MODE" != "off" ]; then
  python /app/server/write_bridge_config.py
  python /app/server/bridge_sidecar.py &
fi
exec uvicorn app.main:app --host 0.0.0.0 --port 8080
