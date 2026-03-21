# GBAsync Server

FastAPI backend for binary save storage and metadata coordination.

## Features (MVP)

- API-key protected endpoints
- Binary save upload/download
- Metadata index with timestamp + hash
- Supports forced overwrite uploads (`force=1`)
- Conflict flag on equal timestamp + different hash
- Atomic write safety
- Optional version-history backup

## Endpoints

- `GET /health`
- `GET /saves`
- `GET /conflicts`
- `GET /save/{game_id}/meta`
- `GET /save/{game_id}`
- `PUT /save/{game_id}`
- `DELETE /save/{game_id}` — remove index row and delete the stored blob (cleanup / bad test data)
- `POST /resolve/{game_id}`

`GET /saves` is driven by the **metadata index** (`INDEX_PATH`, e.g. `save_data/index.json`), not by re-scanning the save folder. Curl uploads register rows there; deleting local test files does **not** remove server metadata. Use `DELETE` or edit the index JSON if entries outlive the files.

## Run

**Docker (from `server/`, repo-root `.env`):** see root `USER_GUIDE.md`. The image entrypoint runs **uvicorn** and optionally a Dropbox **sidecar** when `SAVESYNC_DROPBOX_MODE` is not `off`.

**Local uvicorn:**

```bash
cp ../.env.example ../.env   # once, at repository root
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --reload --host 0.0.0.0 --port 8080
```

## Upload request example

```bash
curl -X PUT "http://127.0.0.1:8080/save/pokemon-emerald?last_modified_utc=2026-03-17T21:00:00%2B00:00&sha256=<sha>&size_bytes=131072&filename_hint=Pokemon%20Emerald.sav&platform_source=delta-bridge" \
  -H "X-API-Key: change-me" \
  --data-binary @Pokemon\ Emerald.sav
```

Delete a save from the index (and remove `pokemon-emerald.sav` under `SAVE_ROOT`):

```bash
curl -X DELETE "http://127.0.0.1:8080/save/pokemon-emerald" -H "X-API-Key: change-me"
```

Force-overwrite example:

```bash
curl -X PUT "http://127.0.0.1:8080/save/pokemon-emerald?last_modified_utc=1970-01-01T00:00:00%2B00:00&sha256=<sha>&size_bytes=131072&filename_hint=Pokemon%20Emerald.sav&platform_source=3ds-homebrew&force=1" \
  -H "X-API-Key: change-me" \
  --data-binary @Pokemon\ Emerald.sav
```
