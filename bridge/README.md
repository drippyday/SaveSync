# Delta Bridge

Desktop sync agent that syncs a **local folder** of `.sav` files with the GBAsync server.

### Is this “Dropbox integration”?

**Partially.** `bridge.py` only reads the filesystem. If Delta (or anything else) stores saves in a folder that **Dropbox’s desktop app** mirrors—e.g. you point `delta_save_dir` at `~/Dropbox/SomeFolder`—then Dropbox is only moving files; **GBAsync still sees plain local `.sav` names**.

Delta’s **built-in** Dropbox/Google sync uses the **Harmony** stack and **does not** expose a simple “folder of `.sav` files” to third-party tools. So you **cannot** point `bridge.py` at Delta’s raw cloud layout on disk in a supported way.

For Dropbox API usage, start with **`DROPBOX_SETUP.md`** (`.env` + JSON paths). Details and Harmony notes: **`DROPBOX.md`**, **`DELTA_DROPBOX_FORMAT.md`**.

## `bridge.py` modes

- `--once`: one pull/push pass then exit
- `--watch`: filesystem watch + periodic polling
- `--dry-run`: print actions without writing/uploading

## Setup

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp config.example.json config.json
python bridge.py --config config.json --once
```

## Optional: Dropbox API sync

```bash
pip install -r requirements-dropbox.txt
# From repo root: cp .env.example .env  then add DROPBOX_* (see DROPBOX_SETUP.md)
python dropbox_bridge.py --config config.example.dropbox.json --once
```

## Delta Dropbox folder ↔ `.sav` (Harmony file layout)

If you download the **Delta Emulator** folder from Dropbox, saves live next to JSON metadata as `GameSave-{id}-gameSave` (see **`DELTA_DROPBOX_FORMAT.md`**).

```bash
python3 delta_dropbox_sav.py list --delta-root "/path/to/Delta Emulator"
python3 delta_dropbox_sav.py export --delta-root "..." --out-dir ./savs
python3 delta_dropbox_sav.py import-sav --delta-root "..." --identifier <40-hex> --sav ./updated.sav --backup-dir ./bak
```

Use that to round-trip with **GBAsync** (export `.sav` → upload via bridge/server; download from server → `import-sav` → re-sync folder to Dropbox). This matches the “search for the id string, download GameSave, rename to `.sav`” guides, but keeps metadata consistent on **import**.

## Server / local `.sav` ↔ Delta Dropbox folder (Harmony conversion)

**Goal:** keep GBAsync (3DS/Switch uploads) in sync with Delta’s **real** Dropbox layout (`GameSave-*` JSON + `*-gameSave` blobs), not a separate flat `.sav` tree in Dropbox. When the server or a local `.sav` wins, the script **pads, hashes, and rewrites** those Harmony files (`apply_bytes_to_delta`); the **Dropbox desktop app** then uploads the changed files.

Run **`delta_folder_server_sync.py`**:

1. `pip install -r requirements.txt`
2. Copy **`config.example.delta_sync.json`** → `config.delta_sync.json`
3. Set **`delta_root`** to the **`Delta Emulator`** folder Dropbox syncs locally, plus **`server_url`**, **`api_key`**, **`rom_dirs`** (and optional **`rom_map_path`**, **`rom_extensions`**, **`delta_slot_map_path`**). Linking to Delta is by **ROM SHA-1** (`Game-*.json` `sha1Hash`); ROMs are scanned recursively under `rom_dirs`.
4. **`sync_mode`**:
   - **`triple`** (default): merge **local `.sav` mtime**, **server**, and **Delta** — use when you also edit plain `.sav` files on disk.
   - **`server_delta`**: merge **only server vs Delta** — use when 3DS/Switch (and the server) are the source of truth; **`local_save_dir`** is just a **mirror** of the winning bytes (e.g. repo **`save_data/saves`** on the host that runs the script).
   - **`delta_slot_map_path`** (recommended): persistent JSON map of **Harmony slot id** (`GameSave-{id}`) → server **`game_id`**. This stabilizes hack/retail routing across runs and avoids accidental remaps when titles share the same cartridge header (`BPRE`, `BPEE`, etc.).
5. Run periodically:

```bash
python3 delta_folder_server_sync.py --config config.delta_sync.json --once
```

Only **`--once`** is built in; use cron/launchd or a loop for repeats.

### Same merge over the Dropbox HTTP API (no desktop client)

Use **`delta_dropbox_api_sync.py`** when the Delta folder exists **only in Dropbox** and you want the [Dropbox HTTP API](https://www.dropbox.com/developers/documentation/http/documentation) (via the Python SDK). Same **repository-root `.env`** as **`DROPBOX_SETUP.md`**; set **`dropbox.remote_delta_folder`** in **`config.example.delta_dropbox_api.json`**. Each pass downloads the Harmony tree, merges, then uploads changed files.

Current behavior for safer Delta sync:

- Upload order is **blob files first**, then `GameSave-*` JSON sidecars.
- After uploading a save blob, sidecar `files[0].versionIdentifier` is aligned to the blob's Dropbox `rev`.
- This keeps Harmony attachment metadata coherent and reduces Delta download/conflict churn from stale revision pointers.

## Config fields (`bridge.py`)

- `server_url`: GBAsync server base URL
- `api_key`: server API key
- `delta_save_dir`: local Delta save folder to monitor
- `poll_seconds`: periodic poll interval
- `rom_dirs`: optional list of ROM directories for ROM-header-based `game_id`
- `rom_map_path`: optional JSON mapping save stem -> ROM path
- `rom_extensions`: optional ROM extensions used in `rom_dirs` matching

## Notes

- Stale **`game_id`** rows on the server (e.g. after experiments) are removed with **`DELETE /save/{game_id}`** — see root **`server/README.md`** / **`USER_GUIDE.md`** (`index.json` is authoritative for **`GET /saves`**).
- Game ID resolution order:
  1. If matching ROM is found, derive from GBA ROM header (`title + game code`)
  2. Fallback to normalized `.sav` filename stem
- ROM matching sources:
  - `rom_map_path` JSON mapping save stem to ROM path
  - `rom_dirs` + `rom_extensions` stem matching
