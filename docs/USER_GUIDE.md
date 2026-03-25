# GBAsync User Guide

This guide explains how to run GBAsync end-to-end across:

- self-hosted server
- desktop Delta bridge
- Nintendo Switch homebrew client
- Nintendo 3DS homebrew client

**Repository-root `.env`:** Every variable supported by **`.env.example`** is documented in **[§2 Environment variables](#2-environment-variables-repository-root-env)** (grouped by area: core API, admin, Dropbox auth, sidecar timing, `plain` vs `delta_api`).

## 1) Start the server

### Option A: Docker (recommended)

```bash
cp .env.example .env   # at repository root (GBAsync/)
cd server
docker compose up -d
```

Saves and the metadata index are stored on the host under **`save_data/`** at the **repository root** (for example `save_data/saves/*.sav` and `save_data/index.json`), bind-mounted into the container—not under `server/`.

**Using a different folder on disk (Syncthing, NAS, cloud drive, etc.):** The server only reads/writes normal files under **`SAVE_ROOT`** and the index paths. You can point Docker’s **volume** (or **`SAVE_ROOT` / `INDEX_PATH` / `HISTORY_ROOT`**) at **any** host directory—for example one that Syncthing or another tool already syncs. GBAsync does not talk to those apps; it only stores files there, and your other software replicates them. See **`server/README.md`** (data directory) and **`bridge/README.md`** (optional **`bridge.py`** mirror of plain `.sav` files).

**Save history (per game):** With **`ENABLE_VERSION_HISTORY=true`**, each time the server replaces a save file, the previous blob is copied under **`save_data/history/{game_id}/`**. **`HISTORY_MAX_VERSIONS_PER_GAME`** (default **5** in Docker Compose; **`0`** = unlimited) caps how many backup files are kept per game. **Pinned** revisions (see below) are **not** counted toward that cap — when trimming, the server drops **unpinned** oldest files first and keeps **`pins.json`** in sync.

List or restore with **`GET /save/{game_id}/history`** and **`POST /save/{game_id}/restore`**, from the **admin** UI (**History** on a row), or from the **homebrew save viewer** (**A** = history / restore). On **Switch** and **3DS** history screens, **R** toggles **keep** (pinned) for the highlighted revision (`PATCH /save/{game_id}/history/revision/keep`); pinned rows show **`[KEEP]`**. Restoring changes the **server** copy only; run **download** (or Auto) on each device afterward so local `.sav` files match, or the next upload may overwrite the restored server file.

**Revision labels:** Optional per-backup names are stored in **`save_data/history/{game_id}/labels.json`**. Set from admin (**label save** in the history sheet) or **`PATCH /save/{game_id}/history/revision`** with `filename` + `display_name`. Listed in **`GET /save/{game_id}/history`** as **`display_name`** on each entry (separate from the main save’s display name).

**Display names:** Optional **`display_name`** in the index (set in admin with **Display name**, or **`PATCH /save/{game_id}/meta`**). Returned in **`GET /saves`**. On **Switch** and **3DS** **save viewer** (main menu **R**), the primary line shows **`display_name`** when set, and falls back to **`game_id`** — it does **not** change the canonical **`game_id`** or routing.

**Dropbox in the same container:** configure **`GBASYNC_DROPBOX_MODE`** and related variables in the **repository-root** **`.env`**. Full variable list, defaults, and which credentials apply to **`plain`** vs **`delta_api`** are in **[§2 Environment variables](#2-environment-variables-repository-root-env)**. **`server/docker-compose.yml`** mounts **`${HOME}/Documents/GBA`** → **`/roms`** (read-only) for ROM hashing; override that volume if your ROMs live elsewhere.

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

## 2) Environment variables (repository-root .env)

Copy **`/.env.example`** → **`.env`** at the **repository root** (same folder as `README.md`). The server loads this file on startup (`server/app/main.py` uses `load_dotenv` on that path). **Docker Compose** also passes **`env_file: ../.env`** from `server/docker-compose.yml`, so the same file configures the container.

**Legacy names:** Every **`GBASYNC_*`** variable also accepts **`SAVESYNC_*`** with the same meaning (e.g. `GBASYNC_DROPBOX_MODE` or `SAVESYNC_DROPBOX_MODE`). Prefer **`GBASYNC_*`** in new configs.

**Paths:** Values like **`../save_data/...`** are written for running **uvicorn from `server/`** (cwd = `server/`). In **Docker**, `server/docker-compose.yml` **`environment:`** overrides **`SAVE_ROOT`**, **`INDEX_PATH`**, **`HISTORY_ROOT`**, and **`HISTORY_MAX_VERSIONS_PER_GAME`** (see that file), bind-mounts **`../save_data`** → **`/data`**, and still loads the rest of your variables from **`.env`** via **`env_file`**. The **`SAVE_*` / `INDEX_*` paths inside `.env`** therefore apply to **local uvicorn**; in Compose, use the **`environment:`** block for in-container paths.

| Variable | Required | Default / if unset | What it does |
|----------|----------|--------------------|--------------|
| **`API_KEY`** | Strongly recommended | — | Shared secret: clients and bridge send **`X-API-Key`** with this value. If **empty or unset**, the server **does not** validate API keys (**open API** — fine only for trusted local testing). |
| **`SAVE_ROOT`** | Yes | `./data/saves` (code default) | Directory where per-game **`.sav`** blobs are stored. |
| **`INDEX_PATH`** | Yes | `./data/index.json` | **`GET /saves`** metadata (**`index.json`**), not a full directory scan. |
| **`HISTORY_ROOT`** | Yes | `./data/history` | Parent folder for per-game version history when enabled. |
| **`ENABLE_VERSION_HISTORY`** | No | `true` | If **`true`**, each replacement of a save can keep prior blobs under **`HISTORY_ROOT/{game_id}/`**. If **`false`**, history features are off. |
| **`HISTORY_MAX_VERSIONS_PER_GAME`** | No | `5` | Max backup **files** per game (**`0`** = unlimited). Pinned revisions are not counted toward the cap. |
| **`SAVE_SYNC_LOG_CLIENT_DEBUG`** | No | off | If **`1`**, **`true`**, or **`yes`**, extra logging when clients **`POST /debug/client-clock`**. |

### Admin web UI

| Variable | Required | Default / if unset | What it does |
|----------|----------|--------------------|--------------|
| **`GBASYNC_ADMIN_PASSWORD`** | No | empty | If **non-empty**, **`/admin`** and **`/admin/ui/`** are enabled; if empty, admin routes are **404**. |
| **`GBASYNC_ADMIN_SECRET`** | No | — | If set, used **only** for signing the admin **session cookie** HMAC. If unset, **`API_KEY`** is used for that HMAC. **`X-API-Key`** for admin APIs still uses **`API_KEY`**. |
| **`GBASYNC_SLOT_MAP_PATH`** | No | — | Optional filesystem path to a **slot map** JSON file; exposed via **`GET /admin/api/slot-map`** when admin is enabled. Also accepts **`SAVESYNC_SLOT_MAP_PATH`**. |

### Dropbox authentication (only when using Dropbox API scripts)

Configure **one** auth method. Scopes typically include **files.metadata.read** and **content read/write** (see **`bridge/DROPBOX_SETUP.md`**).

| Variable | Required | What it does |
|----------|----------|--------------|
| **`DROPBOX_ACCESS_TOKEN`** | One method | Short-lived **user** access token (simplest for testing). |
| **`DROPBOX_APP_KEY`** | Other method | App key for refresh-token flow. |
| **`DROPBOX_APP_SECRET`** | Other method | App secret for refresh-token flow. |
| **`DROPBOX_REFRESH_TOKEN`** | Other method | Long-lived refresh token (typical for servers). If using this trio, you can leave **`DROPBOX_ACCESS_TOKEN`** unset. |

### `GBASYNC_DROPBOX_MODE` and related

| Variable | Required | Default / if unset | What it does |
|----------|----------|--------------------|--------------|
| **`GBASYNC_DROPBOX_MODE`** | No | `off` | **`off`** — no Dropbox bridge in the container; API only. **`plain`** — **`dropbox_bridge.py`** syncs a **flat** `*.sav` folder in Dropbox. **`delta_api`** — **`delta_dropbox_api_sync.py`** syncs Delta’s **Harmony** tree via the Dropbox API. |
| **`GBASYNC_PUBLIC_SERVER_URL`** | When mode ≠ `off` | `http://127.0.0.1:8080` | Base URL written into generated bridge config (**`server_url`**). Must match what your clients use (host/port). |

**Sidecar (Docker entrypoint):** When mode is **`plain`** or **`delta_api`**, the image runs **`write_bridge_config.py`** and **`bridge_sidecar.py`**, which periodically invoke the same scripts as in **`bridge/`**.

| Variable | Required | Default / if unset | What it does |
|----------|----------|--------------------|--------------|
| **`GBASYNC_DROPBOX_INTERVAL_SECONDS`** | No | `300` in code; **`86400`** in **`.env.example`** | Seconds **between** scheduled bridge runs (minimum **10** in sidecar). Use a long value if you rely on upload-triggered sync. |
| **`GBASYNC_BRIDGE_START_DELAY_SECONDS`** | No | `8` | Seconds to wait after container start before the **first** sidecar run. |
| **`GBASYNC_DROPBOX_SYNC_ON_UPLOAD`** | No | **off** if unset | If **`1`/`true`/`yes`**, after a **`PUT`** stores new bytes the server **debounces** and runs one Dropbox bridge pass. **`.env.example`** sets **`true`** for event-driven sync; omit or set **`false`** to rely on the interval only. |
| **`GBASYNC_DROPBOX_SYNC_DEBOUNCE_SECONDS`** | No | `10` | Quiet period after the last qualifying **`PUT`** before that upload-triggered run. |
| **`GBASYNC_DROPBOX_SYNC_TIMEOUT_SECONDS`** | No | `600` | Max seconds for each bridge **subprocess** run (upload-triggered or manual **`POST /dropbox/sync-once`**). |

**Plain mode only:**

| Variable | Required | What it does |
|----------|----------|--------------|
| **`DROPBOX_REMOTE_FOLDER`** | **Yes** for `plain` | Dropbox path to the **flat** `*.sav` folder (leading **`/`**, e.g. **`/GBAsync/gba`**). |
| **`GBASYNC_DROPBOX_POLL_SECONDS`** | No | If set, overrides **`poll_seconds`** in the generated JSON for **`dropbox_bridge.py`**. If unset, **`write_bridge_config`** uses **`GBASYNC_DROPBOX_INTERVAL_SECONDS`** for **`poll_seconds`**. |

**`delta_api` mode:**

| Variable | Required | Default / if unset | What it does |
|----------|----------|--------------------|--------------|
| **`DROPBOX_REMOTE_DELTA_FOLDER`** | **Yes** for `delta_api` | Dropbox path to Delta’s **Harmony** root (e.g. **`/Delta Emulator`**). |
| **`GBASYNC_ROM_DIRS`** | No | — | Comma-separated **container** paths scanned for ROMs (SHA-1 → **`game_id`**). Docker Compose often mounts host ROMs at **`/roms`** and sets **`GBASYNC_ROM_DIRS=/roms`**. |
| **`GBASYNC_ROM_EXTENSIONS`** | No | `.gba` in **`.env.example`**; code default **`.gba,.nds`** | Comma-separated list used when matching ROM files. |
| **`GBASYNC_ROM_MAP_PATH`** | No | — | Optional JSON map from save stems to ROM paths (passed through to generated config). |
| **`GBASYNC_DELTA_SYNC_MODE`** | No | `server_delta` | **`triple`** or **`server_delta`** — passed to **`delta_dropbox_api_sync.py`** (three-way vs server–Delta merge). |
| **`GBASYNC_DELTA_SLOT_MAP_PATH`** | No | `/data/delta-slot-map.json` in generated config | Harmony **slot map** path inside the container. |
| **`GBASYNC_SERVER_DELTA_ONE_WAY`** | No | see below | **`false`** / **`0`** — two-way guardrails (Harmony timestamps can block server→Dropbox). **`true`** / **`1`** — when server and Dropbox bytes differ, prefer pushing server → Delta. If unset or invalid, **`write_bridge_config`** defaults **`server_delta_one_way`** to **`true`** (see **`server/write_bridge_config.py`**). |
| **`GBASYNC_SERVER_DELTA_MIN_DELTA_WIN_SECONDS`** | No | — | Guardrail: minimum seconds before Delta’s **`modifiedDate`** can “win” in two-way flows (integer ≥ 0). |
| **`GBASYNC_SERVER_DELTA_RECENT_SERVER_PROTECT_SECONDS`** | No | — | Guardrail: recent server uploads protected for this many seconds (integer ≥ 0). |

For behavior of the merge and first-time conflicts, see **§ Delta two-way behavior** under **[3) Configure and run Delta bridge](#3-configure-and-run-delta-bridge)**.

---

## 3) Configure and run Delta bridge

1. Copy `bridge/config.example.json` to `bridge/config.json`
2. Set:
   - `server_url`
   - `api_key`
   - `delta_save_dir` — a **local** folder that contains plain `*.sav` files (often Delta’s GBA save directory on Mac; can live under a **Dropbox-synced** path if the desktop client keeps real `.savs` there).

The bridge does **not** talk to Delta’s **in-app** Dropbox/Google sync (Harmony uses its own cloud layout, not a flat `.sav` folder). For a **flat** `*.sav` folder in Dropbox via the API, use **`dropbox_bridge.py`**. For Delta’s **Harmony** tree over the API, use **`delta_dropbox_api_sync.py`** (see **`bridge/DROPBOX.md`**).

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

## 4) Install Switch client

Use artifact:

- `dist/switch/gbasync-switch-v1.0.0/gbasync.nro`

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

**Main menu:** **A** Auto sync, **X** upload-only, **Y** download-only, **+** exits the app. A short **status line** shows last sync / server / Dropbox (from **`.gbasync-status`** next to your saves). When applicable, **`N game(s) need sync (run Auto)`** summarizes games that still need an Auto action; that count is **cached** after it is computed once and **refreshes** when you return from Auto sync, upload-only, download-only, Dropbox sync, or the **save viewer** (not on every menu redraw).

**Auto sync** runs a **preview** of planned actions per game (upload/download/**conflict**/locked — **non-OK rows only**; **no-baseline** divergences show as **CONFLICT**, not SKIP). **A** applies; **B** cancels; **+** (**Plus**) applies and **exits the app**. Preview rows use **Display name** when the server sent one; otherwise **`game_id`**. **+** does not cancel the preview when confirming with **A** (avoids accidental backs). Preview is **confirm-only** (no lock editing there). Change **locks** from the main menu **Save viewer** (**R**): highlight a row and **R** toggles lock; **`locked_ids=`** is written to `sdmc:/switch/gba-sync/config.ini`. The viewer lists **Display name** when the server has one; otherwise **`game_id`**. **A** opens **history / restore** (**R** in history toggles **keep**).

After logs finish, a **done** screen appears: **A** returns to the main menu; **+** exits the app.

Expected in-app output example:

```text
Local saves: 3
Remote saves: 2
pokemon-emerald: UPLOADED
metroid-zero: DOWNLOADED
```

## 5) Install 3DS client

Use artifact:

- `dist/3ds/gbasync-3ds-v1.0.0/gbasync.3dsx`

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

**Main menu:** **A** / **X** / **Y** as labeled; **START** exits to the “Press START to exit” end screen (or exits immediately if you already chose **START** on the post-sync screen). A **status line** shows last sync / server / Dropbox (from **`.gbasync-status`** in the active save folder). When applicable, **`N game(s) need sync (run Auto)`** uses the same **caching** rules as Switch (see above).

**Auto sync** shows a **preview** of planned actions (**non-OK rows only**), then **A** applies, **B** cancels, or **START** applies and **exits the app**. Preview lists **Display name** when available; otherwise **`game_id`**. Preview is **confirm-only**. **Save viewer** (main menu **R**): **R** toggles lock for the highlighted row and updates **`sdmc:/3ds/gba-sync/config.ini`** (`locked_ids=`). Rows show **Display name** when the server has one; otherwise **`game_id`**. **A** opens history (**R** in history toggles **keep**).

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

## 6) Validate cross-device sync

1. Run one client (Delta/Switch/3DS) and make save progress.
2. On a console with newer progress, press **`A`** (Auto sync): **Switch** and **3DS** both use each game’s save **SHA256** and a small **`.gbasync-baseline`** file next to your `.sav` files (legacy **`.savesync-baseline`** is still supported; unreliable SD modification times are not used for merge decisions on either console). Or use upload-only (`X`) to force-push chosen saves.
3. On the other console, press **`A`** again or download-only (`Y`) to pull saves from the server.

If a game exists **both locally and on the server** with **different hashes** but **no baseline row** for that game yet (typical first sync on a device), **Auto** opens the **Conflict** screen instead of logging SKIP: **`X`** uploads local (with force), **`Y`** downloads from the server, **`B`** skips. The **first-time** copy explains that there is **no sync history on this device yet**; pick **X** or **Y** to seed **`.gbasync-baseline`**. If **local and server both diverged** from an **existing** baseline, the same **X/Y/B** screen appears with wording about **both sides changing since the last successful sync**. **Local-only** or **server-only** games still **upload** or **download** without this prompt.

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

## 7) Server index vs files (`index.json`)

The server’s **`GET /saves`** list is driven by the **metadata index** (default host path **`save_data/index.json`** when using repo Docker layout), not by scanning `save_data/saves/` alone. Uploads (curl, bridge, consoles) **register rows** in that index. The JSON response includes **`total`** (row count). Optional query parameters **`limit`** and **`offset`** paginate the list for large libraries (see **`server/README.md`**). **`PUT /save/{game_id}`** rejects bodies larger than **`GBASYNC_MAX_SAVE_UPLOAD_BYTES`** (default 4 MiB).

If you **delete local test `.sav` files** or remove blobs on disk but leave the index alone, clients can still **see** those `game_id`s and hit **404** or upload errors until you:

- Call **`DELETE /save/{game_id}`** (API key required), or  
- Manually edit **`index.json`** and remove the stale keys (and delete orphaned `*.sav` if any).

See **`server/README.md`** for the `DELETE` curl example.

## 8) Common issues

- **401 unauthorized**: API key mismatch.
- **No saves found**: wrong `save_dir`, missing `.sav` files.
- **Cannot connect**: wrong server IP/port or firewall block.
- **No cross-device update**: use `X` on source device, then `Y` on destination device.
- **Conflicts**: check `GET /conflicts`, resolve via `POST /resolve/{game_id}`.
- **“Ghost” saves after tests**: stale **`index.json`** rows — use **`DELETE /save/{game_id}`** or edit the index.
- **Switch preview screen**: use **A** to run the planned sync; **B** backs out; **+** does not cancel there (avoids accidental backs).

## 9) Game ID normalization

Bridge game ID resolution order:

1. ROM header (`title + game code`) when a matching ROM is found
2. Fallback to normalized save filename stem

To improve cross-device matching accuracy, set in bridge config:

- `rom_dirs`: list of directories containing `.gba` ROMs
- `rom_map_path`: optional JSON map `{ "Save File Stem": "/full/path/to/ROM.gba" }`
- `rom_extensions`: ROM extensions for auto-matching (default `[".gba"]`)

## 10) Current MVP limits

- Switch/3DS clients use **HTTP** only in this repo path (no TLS).
- **`game_id`:** ROM header when `[rom]` paths are set on the console config; otherwise normalized save stem (same idea as the bridge).
- Console sync is manual (app-triggered), not background.

## 11) Dist artifact reference

### `dist/server`

- **`gbasync-server-vX.Y.Z.zip`**: Contains **`gbasync-server-vX.Y.Z/`** with the Docker **`.tar`**, **`docker-compose.yml`**, **`.env`**, **`.env.example`**, and **`README.md`** (install / volumes).
- **`gbasync-server-vX.Y.Z.tar`**: Docker image archive (inside the folder above).

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

- **`gbasync-switch-vX.Y.Z.zip`**: Same layout as the **`gbasync-switch-vX.Y.Z/`** folder.
- `gbasync.nro`: runnable Switch homebrew app (what users launch).
- `gbasync.nacp`: app metadata (title/author/version), consumed by tooling.
- `gbasync.elf`: raw executable/debug build artifact (not typically copied to SD for normal use).
- **`README.md`**: install steps at package root.
- **`gba-sync/config.ini`**: template config (path on SD: **`sdmc:/switch/gba-sync/config.ini`**).
- **`gba-sync/README.md`**: every **`config.ini`** key for Switch.

### `dist/3ds`

- **`gbasync-3ds-vX.Y.Z.zip`**: Same layout as **`gbasync-3ds-vX.Y.Z/`**.
- `gbasync.3dsx`: runnable 3DS homebrew app (what users launch).
- `gbasync.smdh`: icon/metadata asset used by Homebrew Launcher.
- **`README.md`**, **`gba-sync/config.ini`**, **`gba-sync/README.md`** (config path: **`sdmc:/3ds/gba-sync/config.ini`**).
