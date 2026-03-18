# SaveSync Server

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
- `POST /resolve/{game_id}`

## Run

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
uvicorn app.main:app --reload --host 0.0.0.0 --port 8080
```

## Upload request example

```bash
curl -X PUT "http://127.0.0.1:8080/save/pokemon-emerald?last_modified_utc=2026-03-17T21:00:00%2B00:00&sha256=<sha>&size_bytes=131072&filename_hint=Pokemon%20Emerald.sav&platform_source=delta-bridge" \
  -H "X-API-Key: change-me" \
  --data-binary @Pokemon\ Emerald.sav
```

Force-overwrite example:

```bash
curl -X PUT "http://127.0.0.1:8080/save/pokemon-emerald?last_modified_utc=1970-01-01T00:00:00%2B00:00&sha256=<sha>&size_bytes=131072&filename_hint=Pokemon%20Emerald.sav&platform_source=3ds-homebrew&force=1" \
  -H "X-API-Key: change-me" \
  --data-binary @Pokemon\ Emerald.sav
```
