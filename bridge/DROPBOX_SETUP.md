# Dropbox API setup (one `.env`)

Scripts use the Dropbox **HTTP API** via the Python SDK:  
[Dropbox HTTP documentation](https://www.dropbox.com/developers/documentation/http/documentation).

Put credentials in the **repository-root** `.env` (same file as the GBAsync server). The server ignores `DROPBOX_*`.

For a **standalone bridge zip**, use `.env` in the same folder as the scripts.

**`server_delta` / Delta API sync does not add new games to Dropbox.** It only updates Harmony files for titles **already** in your Delta library (you’ve opened that ROM in Delta at least once so `Game-*` / `GameSave-*` exist). There is no “push FireRed to Dropbox from the server” path until Delta has created that entry. Then your local ROM file must have the **same SHA-1** Delta stored for that game.

### Docker (same container as the server)

If you run **`docker compose`** from `server/`, you can enable Dropbox without installing bridge software on the host:

1. In the root `.env`, set e.g. `SAVESYNC_DROPBOX_MODE=delta_api`, `DROPBOX_ACCESS_TOKEN=…`, `DROPBOX_REMOTE_DELTA_FOLDER=/…`, and mount ROMs + set `SAVESYNC_ROM_DIRS=/roms` when using Delta API sync (see comments in `.env.example` and `server/docker-compose.yml`).
2. `docker compose up -d --build`

The entrypoint starts **uvicorn** and a small **sidecar** that runs the chosen bridge on `SAVESYNC_DROPBOX_INTERVAL_SECONDS`.

For stable Delta two-way mode (`SAVESYNC_SERVER_DELTA_ONE_WAY=false`), recommended starting values:

```env
SAVESYNC_DROPBOX_INTERVAL_SECONDS=120
SAVESYNC_SERVER_DELTA_MIN_DELTA_WIN_SECONDS=900
SAVESYNC_SERVER_DELTA_RECENT_SERVER_PROTECT_SECONDS=3600
```

One first-time conflict prompt in Delta can still happen after earlier divergent history; choose the side you want once, then normal sync should stabilize.

---

## 1. Install dependencies

```bash
cd bridge
pip install -r requirements-dropbox.txt
```

---

## 2. Dropbox app + token (pick one)

### A) Generated access token — **easiest for your own Dropbox**

1. [App Console](https://www.dropbox.com/developers/apps) → your app → **Permissions**: enable `files.metadata.read`, `files.content.read`, `files.content.write` (and match **App folder** vs **Full Dropbox** to how you’ll set paths in JSON).
2. **Settings** tab → **OAuth 2** → **Generated access token** → **Generate**.
3. In root `.env` set **only**:

   ```env
   DROPBOX_ACCESS_TOKEN=paste_token_here
   ```

You do **not** need a refresh token for this path. The token is for **your** linked account (fine for self-hosted GBAsync).

### B) App key + secret + **refresh** token — **better for automation**

Dropbox **does** issue refresh tokens: you must add **`token_access_type=offline`** to the [authorization URL](https://www.dropbox.com/developers/documentation/http/documentation#oauth2-authorize), then exchange the code at `/oauth2/token` — see the official [OAuth guide → “Using refresh tokens”](https://developers.dropbox.com/oauth-guide).

Then in `.env`:

```env
DROPBOX_APP_KEY=...
DROPBOX_APP_SECRET=...
DROPBOX_REFRESH_TOKEN=...
```

(Leave `DROPBOX_ACCESS_TOKEN` unset if you use this.)

---

## 3. GBAsync + Dropbox paths in `.env`

You should already have server lines (`API_KEY`, `SAVE_ROOT`, …). Add either `DROPBOX_ACCESS_TOKEN` or the three refresh variables as above.

```bash
# from repo root, if needed:
cp .env.example .env
```

**Do not commit `.env`.**

---

## 4. JSON config (paths only — not secrets)

| Goal | Start from | Dropbox field |
|------|------------|----------------|
| Plain `*.sav` folder ↔ server | `config.example.dropbox.json` | `dropbox.remote_folder` |
| Delta Harmony folder ↔ server (API) | `config.example.delta_dropbox_api.json` | `dropbox.remote_delta_folder` |

Set `server_url` and `api_key` to match your GBAsync server. Paths must start with `/` (e.g. `/Delta Emulator` for an app-folder app).

---

## 5. Run

From **`bridge/`**, after copying a config to e.g. `config.dropbox.json` or `config.delta_dropbox_api.json`:

**Plain `.sav` folder in Dropbox:**

```bash
cd bridge
python3 dropbox_bridge.py --config config.dropbox.json --once
```

**Delta Emulator tree over the API:**

```bash
cd bridge
python3 delta_dropbox_api_sync.py --config config.delta_dropbox_api.json --once
```

Use `--watch` instead of `--once` on `dropbox_bridge.py` if you want a loop.

More detail: **`DROPBOX.md`**, **`DELTA_DROPBOX_FORMAT.md`**.
