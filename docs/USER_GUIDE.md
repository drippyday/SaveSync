# GBAsync User Guide

This guide explains how to run GBAsync end-to-end across:

- self-hosted server
- desktop Delta bridge
- Nintendo Switch homebrew client
- Nintendo 3DS homebrew client

## 1) Start the server

### Option A: Docker (recommended)

```bash
cp .env.example .env   # at repository root (GBAsync/)
cd server
docker compose up -d
```

Saves and the metadata index are stored on the host under **`save_data/`** at the **repository root** (for example `save_data/saves/*.sav` and `save_data/index.json`), bind-mounted into the container—not under `server/`.

**Save history (per game):** With **`ENABLE_VERSION_HISTORY=true`**, each time the server replaces a save file, the previous blob is copied under **`save_data/history/{game_id}/`**. **`HISTORY_MAX_VERSIONS_PER_GAME`** (default **5** in Docker Compose; **`0`** = unlimited) caps how many backup files are kept per game. **Pinned** revisions (see below) are **not** counted toward that cap — when trimming, the server drops **unpinned** oldest files first and keeps **`pins.json`** in sync.

List or restore with **`GET /save/{game_id}/history`** and **`POST /save/{game_id}/restore`**, from the **admin** UI (**History** on a row), or from the **homebrew save viewer** (**A** = history / restore). On **Switch** and **3DS** history screens, **R** toggles **keep** (pinned) for the highlighted revision (`PATCH /save/{game_id}/history/revision/keep`); pinned rows show **`[KEEP]`**. Restoring changes the **server** copy only; run **download** (or Auto) on each device afterward so local `.sav` files match, or the next upload may overwrite the restored server file.

**Revision labels:** Optional per-backup names are stored in **`save_data/history/{game_id}/labels.json`**. Set from admin (**label save** in the history sheet) or **`PATCH /save/{game_id}/history/revision`** with `filename` + `display_name`. Listed in **`GET /save/{game_id}/history`** as **`display_name`** on each entry (separate from the main save’s display name).

**Display names:** Optional **`display_name`** in the index (set in admin with **Display name**, or **`PATCH /save/{game_id}/meta`**). Returned in **`GET /saves`**. On **Switch** and **3DS** **save viewer** (main menu **R**), the primary line shows **`display_name`** when set, and falls back to **`game_id`** — it does **not** change the canonical **`game_id`** or routing.

**Dropbox in the same container:** set in your **repository-root** `.env`:

- `GBASYNC_DROPBOX_MODE=off` — default; API only.
- `GBASYNC_DROPBOX_MODE=delta_api` — runs **`delta_dropbox_api_sync.py`** on an interval (Harmony folder over the Dropbox API). Set `DROPBOX_REMOTE_DELTA_FOLDER`, `DROPBOX_ACCESS_TOKEN`, and **`GBASYNC_ROM_DIRS=/roms`**. By default **`server/docker-compose.yml`** mounts **`${HOME}/Documents/GBA`** on the host to **`/roms`** in the container; change that volume if your ROMs live elsewhere.
- `GBASYNC_DROPBOX_MODE=plain` — **`dropbox_bridge.py`** for a flat `*.sav` Dropbox folder; set `DROPBOX_REMOTE_FOLDER`.

Tune `GBASYNC_DROPBOX_INTERVAL_SECONDS` (and optional `GBASYNC_BRIDGE_START_DELAY_SECONDS`). For mostly event-driven behavior, enable `GBASYNC_DROPBOX_SYNC_ON_UPLOAD=true`, set `GBASYNC_DROPBOX_SYNC_DEBOUNCE_SECONDS` (default 10), and keep a long interval as fallback. No separate bridge install is required when using Docker.

Verify:

```bash
curl http://127.0.0.1:8080/health
```

Expected response:

```json
{"status":"ok"}
```

### Option B: Python directly

```bash
cp .env.example .env   # at repository root (if needed)
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --host 0.0.0.0 --port 8080
```

Expected startup log snippet:

```text
INFO:     Uvicorn running on http://0.0.0.0:8080
```

## 2) Configure and run Delta bridge

1. Copy `bridge/config.example.json` to `bridge/config.json`
2. Set:
   - `server_url`
   - `api_key`
   - `delta_save_dir` — a **local** folder that contains plain `*.sav` files (often Delta’s GBA save directory on Mac; can live under a **Dropbox-synced** path if the desktop client keeps real `.savs` there).

The bridge does **not** talk to Delta’s **in-app** Dropbox/Google sync (Harmony uses its own cloud layout, not a flat `.sav` folder). For syncing a **Dropbox-only** folder via the API, see **`bridge/DROPBOX.md`** and `dropbox_bridge.py`.

Run once:

```bash
cd bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python bridge.py --config config.json --once
```

Expected one-shot output example:

```text
[upload] Pokemon Emerald.sav -> pokemon-emerald
```

Continuous watch mode:

```bash
python bridge.py --config config.json --watch
```

Expected watch-mode output example:

```text
[watch] started
[download] pokemon-emerald -> Pokemon Emerald.sav
```

### Optional: Dropbox folder ↔ server (`dropbox_bridge.py`)

Use this when your saves live in a **Dropbox directory you control** (exports, copies from hardware, etc.), not inside Delta’s managed sync format.

1. Follow **`bridge/DROPBOX_SETUP.md`** (`.env` + app + refresh token).
2. `pip install -r requirements-dropbox.txt`
3. Copy `config.example.dropbox.json`, set `dropbox.remote_folder`, then:

```bash
cd bridge
python3 dropbox_bridge.py --config config.dropbox.json --once
```

### Optional: GBAsync ↔ Delta's real Dropbox folder (`delta_folder_server_sync.py`)

Use this to **write the newest saves into Delta's Harmony files** (`GameSave-*` + `*-gameSave`) on disk, so Delta on iOS sees the same data as your **GBAsync server** (after 3DS/Switch sync). This is **not** `dropbox_bridge.py` (no flat `.sav` folder in the Dropbox API).

- **`sync_mode`: `triple`** — three-way merge: local `.sav` **mtime**, server, and Delta `modifiedDate`.
- **`sync_mode`: `server_delta`** — only **server vs Delta**; set when devices upload to the server and you want that copy to **replace** the Delta blob when newer (and optionally mirror to `local_save_dir`, e.g. `save_data/saves` on the machine running the script).

ROM **SHA-1** in Delta’s `Game-*.json` must match a ROM file under `rom_dirs` (or `rom_map_path`) so each Delta title maps to the correct `game_id`.

1. Copy `bridge/config.example.delta_sync.json` to e.g. `bridge/config.delta_sync.json`.
2. Set `delta_root`, `server_url`, `api_key`, ROM paths, and `sync_mode` as above; set `local_save_dir` for `triple` or as a mirror for `server_delta`.
3. From `bridge` with `requirements.txt` installed:

```bash
python3 delta_folder_server_sync.py --config config.delta_sync.json --once
```

Run on a schedule if you want it to stay current. Leave the **Dropbox desktop client** running so modified Delta files upload.

**Dropbox API (no desktop app):** `delta_dropbox_api_sync.py` + `config.example.delta_dropbox_api.json`; put **`DROPBOX_*`** in the **repository-root** `.env` (same file as the server) — see **`bridge/DROPBOX_SETUP.md`**.

### Delta two-way behavior and first conflict

If Delta and server histories already diverged, Delta can show a **one-time conflict chooser** the first time you reconnect the flows. Choose the side you want (typically **Cloud** right after a 3DS/Switch upload), then normal Auto sync should settle.

For stable two-way (`GBASYNC_SERVER_DELTA_ONE_WAY=false`), use guardrails like:

```env
GBASYNC_DROPBOX_INTERVAL_SECONDS=120
GBASYNC_SERVER_DELTA_MIN_DELTA_WIN_SECONDS=900
GBASYNC_SERVER_DELTA_RECENT_SERVER_PROTECT_SECONDS=3600
```

`delta_dropbox_api_sync.py` also aligns `GameSave` `versionIdentifier` to the blob's Dropbox revision and uploads blobs before sidecars, which keeps Delta attachment metadata consistent.

**Save size (mGBA vs Delta):** mGBA on Switch/3DS often produces **131088**-byte GBA flash saves (128 KiB plus a 16-byte footer). Delta’s Harmony slot for the same game is usually **131072** bytes. When the bridge writes **from the GBAsync server into Delta’s Dropbox files**, it **trims those 16 bytes** only in that step so the blob matches Delta’s `files[0].size`. Your server copy is unchanged; see **`bridge/DELTA_DROPBOX_FORMAT.md`** for detail.

## 3) Install Switch client

Use artifact:

- `dist/switch/gbasync-switch-v0.1.8/gbasync.nro`

On SD card:

- `sdmc:/switch/gbasync.nro`
- `sdmc:/switch/gba-sync/config.ini`

Config:

```ini
[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
save_dir=sdmc:/mGBA
# Optional: comma-separated game_id list skipped on Auto (no upload/download)
# locked_ids=my-hack-title,other-game

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

Launch from Homebrew Menu.

**Main menu:** **A** Auto sync, **X** upload-only, **Y** download-only, **+** exits the app. A short **status line** shows last sync / server / Dropbox (from **`.gbasync-status`** next to your saves).

**Auto sync** runs a **preview** of planned actions per game (upload/download/skip/conflict/locked — **non-OK rows only**), then **A** applies or **B** returns to the menu (**+** does not cancel on the preview — avoids accidental backs). Preview is **confirm-only** (no lock editing there). Change **locks** from the main menu **Save viewer** (**R**): highlight a row and **R** toggles lock; **`locked_ids=`** is written to `sdmc:/switch/gba-sync/config.ini`. The viewer lists **Display name** when the server has one; otherwise **`game_id`**. **A** opens **history / restore** (**R** in history toggles **keep**).

After logs finish, a **done** screen appears: **A** returns to the main menu; **+** exits the app.

Expected in-app output example:

```text
Local saves: 3
Remote saves: 2
pokemon-emerald: UPLOADED
metroid-zero: DOWNLOADED
```

## 4) Install 3DS client

Use artifact:

- `dist/3ds/gbasync-3ds-v0.1.8/gbasync.3dsx`

On SD card:

- `sdmc:/3ds/gbasync.3dsx`
- `sdmc:/3ds/gba-sync/config.ini`

Config:

```ini
[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/mGBA
vc_save_dir=sdmc:/3ds/Checkpoint/saves
# locked_ids=my-hack-title,other-game

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

Launch from Homebrew Launcher.

**Main menu:** **A** / **X** / **Y** as labeled; **START** exits to the “Press START to exit” end screen (or exits immediately if you already chose **START** on the post-sync screen). A **status line** shows last sync / server / Dropbox (from **`.gbasync-status`** in the active save folder).

**Auto sync** shows a **preview** of planned actions per game (**non-OK rows only**), then **A** applies or **B** / **START** returns to the menu. Preview is **confirm-only**. **Save viewer** (main menu **R**): **R** toggles lock for the highlighted row and updates **`sdmc:/3ds/gba-sync/config.ini`** (`locked_ids=`). Rows show **Display name** when the server has one; otherwise **`game_id`**. **A** opens history (**R** in history toggles **keep**).

After sync logs, a **done** screen appears: **A** returns to the main menu; **START** exits the app (skips the duplicate exit prompt).

3DS mode options:

- `mode=normal`: sync plain `.sav` files in `save_dir`
- `mode=vc`: sync saves from `vc_save_dir` (intended for VC export/import workflows)

### 3DS VC inject workflow

For VC inject titles, GBAsync does not write directly into Nintendo save archives.
Use this cycle:

1. Export VC save with your save manager (for example, Checkpoint).
2. Ensure exported `.sav` lands under `vc_save_dir`.
3. Run GBAsync 3DS client with `mode=vc`.
4. Sync other device(s).
5. Export/pull latest save back into `vc_save_dir`.
6. Import save back into VC title via save manager.

Expected in-app output example:

```text
Scanning local saves...
Local saves: 2
Remote saves: 3
pokemon-emerald: DOWNLOADED
```

## 5) Validate cross-device sync

1. Run one client (Delta/Switch/3DS) and make save progress.
2. On a console with newer progress, press **`A`** (Auto sync): **Switch** and **3DS** both use each game’s save **SHA256** and a small **`.gbasync-baseline`** file next to your `.sav` files (legacy **`.savesync-baseline`** is still supported; unreliable SD modification times are not used for merge decisions on either console). Or use upload-only (`X`) to force-push chosen saves.
3. On the other console, press **`A`** again or download-only (`Y`) to pull saves from the server.

The first time a game has no baseline row yet, Auto sync logs **SKIP (no baseline yet)** — use **`X`** or **`Y`** once for that game so Auto can track changes afterward. If **local and server both diverged** from the last known baseline, **3DS** and **Switch** show a **Conflict** screen: **`X`** uploads local (with force), **`Y`** downloads from server, **`B`** skips for now.

After a run, read the on-screen log, then use the **done** prompt (**A** / **+** on Switch; **A** / **START** on 3DS) so the menu does not clear immediately.

4. Confirm same progress appears on second client.
5. Verify metadata via:

```bash
curl -H "X-API-Key: change-me" http://YOUR_SERVER_IP:8080/saves
```

Quick local smoke test (server + bridge only):

```bash
./scripts/smoke-sync.sh
```

## 6) Server index vs files (`index.json`)

The server’s **`GET /saves`** list is driven by the **metadata index** (default host path **`save_data/index.json`** when using repo Docker layout), not by scanning `save_data/saves/` alone. Uploads (curl, bridge, consoles) **register rows** in that index.

If you **delete local test `.sav` files** or remove blobs on disk but leave the index alone, clients can still **see** those `game_id`s and hit **404** or upload errors until you:

- Call **`DELETE /save/{game_id}`** (API key required), or  
- Manually edit **`index.json`** and remove the stale keys (and delete orphaned `*.sav` if any).

See **`server/README.md`** for the `DELETE` curl example.

## 7) Common issues

- **401 unauthorized**: API key mismatch.
- **No saves found**: wrong `save_dir`, missing `.sav` files.
- **Cannot connect**: wrong server IP/port or firewall block.
- **No cross-device update**: use `X` on source device, then `Y` on destination device.
- **Conflicts**: check `GET /conflicts`, resolve via `POST /resolve/{game_id}`.
- **“Ghost” saves after tests**: stale **`index.json`** rows — use **`DELETE /save/{game_id}`** or edit the index.
- **Switch preview screen**: use **A** to run the planned sync; **B** backs out; **+** does not cancel there (avoids accidental backs).

## 8) Game ID normalization

Bridge game ID resolution order:

1. ROM header (`title + game code`) when a matching ROM is found
2. Fallback to normalized save filename stem

To improve cross-device matching accuracy, set in bridge config:

- `rom_dirs`: list of directories containing `.gba` ROMs
- `rom_map_path`: optional JSON map `{ "Save File Stem": "/full/path/to/ROM.gba" }`
- `rom_extensions`: ROM extensions for auto-matching (default `[".gba"]`)

## 9) Current MVP limits

- Switch/3DS clients use **HTTP** only in this repo path (no TLS).
- **`game_id`:** ROM header when `[rom]` paths are set on the console config; otherwise normalized save stem (same idea as the bridge).
- Console sync is manual (app-triggered), not background.

## 10) Dist artifact reference

### `dist/server`

- `gbasync-server-vX.Y.Z.tar`: Docker image archive for the server.
- `docker-compose.yml`: Compose file to run server container.
- `README.txt`: quick load/run instructions for that packaged image.

### `dist/bridge`

- `gbasync-bridge-vX.Y.Z.zip`: distributable desktop bridge package.
- `bridge.py`: bridge app entry point.
- `delta_folder_server_sync.py`: optional local `.sav` ↔ server ↔ Delta Dropbox folder sync.
- `delta_dropbox_api_sync.py`: same merge, but download/upload Delta’s Harmony folder via Dropbox API.
- `config.example.json`: bridge config template.
- `config.example.delta_sync.json`: triple-sync config template.
- `config.example.delta_dropbox_api.json`: Delta folder + GBAsync via Dropbox API.
- `requirements.txt`: Python deps for bridge runtime.
- `run-once.sh`: helper script for one-shot sync.
- `run-watch.sh`: helper script for continuous watch mode.
- `README.md`: bridge-specific notes.

### `dist/switch`

- `gbasync.nro`: runnable Switch homebrew app (what users launch).
- `gbasync.nacp`: app metadata (title/author/version), consumed by tooling.
- `gbasync.elf`: raw executable/debug build artifact (not typically copied to SD for normal use).
- `INSTALL.txt`: end-user install instructions.

### `dist/3ds`

- `gbasync.3dsx`: runnable 3DS homebrew app (what users launch).
- `gbasync.smdh`: icon/metadata asset used by Homebrew Launcher.
- `INSTALL.txt`: end-user install instructions.
