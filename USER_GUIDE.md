# SaveSync User Guide

This guide explains how to run SaveSync end-to-end across:

- self-hosted server
- desktop Delta bridge
- Nintendo Switch homebrew client
- Nintendo 3DS homebrew client

## 1) Start the server

### Option A: Docker (recommended)

```bash
cd server
cp .env.example .env
docker compose up -d
```

Saves and the metadata index are stored on the host under **`save_data/`** at the **repository root** (for example `save_data/saves/*.sav` and `save_data/index.json`), bind-mounted into the container—not under `server/`.

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
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
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
   - `delta_save_dir`

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

## 3) Install Switch client

Use artifact:

- `dist/switch/savesync-switch-v0.1.1/gbasync.nro`

On SD card:

- `sdmc:/switch/gbasync.nro`
- `sdmc:/switch/gba-sync/config.ini`

Config:

```ini
[server]
url=http://YOUR_SERVER_IP:8080
api_key=change-me

[sync]
save_dir=sdmc:/roms/gba/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

Launch from Homebrew Menu.

**Main menu:** **A** full sync, **X** upload-only, **Y** download-only, **+** exits the app.

**Full sync** opens a **confirm** screen: **A** runs Auto sync, **B** returns to the menu (**+** does not cancel here — avoids accidental backs).

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

- `dist/3ds/savesync-3ds-v0.1.1/gbasync.3dsx`

On SD card:

- `sdmc:/3ds/gbasync.3dsx`
- `sdmc:/3ds/gba-sync/config.ini`

Config:

```ini
[server]
url=http://YOUR_SERVER_IP:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/saves
vc_save_dir=sdmc:/3ds/Checkpoint/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

Launch from Homebrew Launcher.

**Main menu:** **A** / **X** / **Y** as labeled; **START** exits to the “Press START to exit” end screen (or exits immediately if you already chose **START** on the post-sync screen).

**Full sync** uses a **confirm** screen (**A** continue, **B** / **START** back to menu — same pattern as other confirms in the app).

After sync logs, a **done** screen appears: **A** returns to the main menu; **START** exits the app (skips the duplicate exit prompt).

3DS mode options:

- `mode=normal`: sync plain `.sav` files in `save_dir`
- `mode=vc`: sync saves from `vc_save_dir` (intended for VC export/import workflows)

### 3DS VC inject workflow

For VC inject titles, SaveSync does not write directly into Nintendo save archives.
Use this cycle:

1. Export VC save with your save manager (for example, Checkpoint).
2. Ensure exported `.sav` lands under `vc_save_dir`.
3. Run SaveSync 3DS client with `mode=vc`.
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
2. On a console with newer progress, press **`A`** (full sync): **Switch** and **3DS** both use each game’s save **SHA256** and a small **`.savesync-baseline`** file next to your `.sav` files (unreliable SD modification times are not used for merge decisions on either console). Or use upload-only (`X`) to force-push chosen saves.
3. On the other console, press **`A`** again or download-only (`Y`) to pull saves from the server.

The first time a game has no baseline row yet, full sync logs **SKIP (no baseline yet)** — use **`X`** or **`Y`** once for that game so Auto can track changes afterward. If **local and server both diverged** from the last known baseline, **3DS** and **Switch** show a **Conflict** screen: **`X`** uploads local (with force), **`Y`** downloads from server, **`B`** skips for now.

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
- **Switch confirm goes to menu**: use **A** to confirm; **B** backs out; do not expect **+** to cancel on the confirm screen.

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

- `savesync-server-vX.Y.Z.tar`: Docker image archive for the server.
- `docker-compose.yml`: Compose file to run server container.
- `README.txt`: quick load/run instructions for that packaged image.

### `dist/bridge`

- `savesync-bridge-vX.Y.Z.zip`: distributable desktop bridge package.
- `bridge.py`: bridge app entry point.
- `config.example.json`: bridge config template.
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
