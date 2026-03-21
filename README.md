# GBAsync

Cross-platform GBA save synchronization across:

- Nintendo Switch (homebrew)
- Nintendo 3DS (homebrew)
- Delta on iOS (through a desktop bridge)

GBAsync lets you continue the same GBA game on different devices by syncing `.sav` files through a self-hosted server.

## Why This Project

Delta has cloud sync, but it is Delta-specific. GBAsync provides one shared sync system across multiple emulators/hardware targets with a consistent server-side source of truth.

## Architecture

GBAsync uses a client-server model:

- `server/`: FastAPI backend that stores save binaries + metadata
- `bridge/`: desktop sync bridge for Delta save folders
- `switch-client/`: Switch homebrew sync app (`.nro`)
- `3ds-client/`: 3DS homebrew sync app (`.3dsx`)

Clients support explicit overwrite sync actions for predictable cross-device transfer.

## Current Status

- [x] Server MVP
- [x] Delta bridge MVP
- [x] Switch client MVP
- [x] 3DS client MVP
- [x] Release/packaging scripts
- [x] End-user installation guides

## Quick Start

### 1) Start server

```bash
cp .env.example .env   # repository root — server + optional Dropbox API keys
cd server
docker compose up -d
```

With Docker, binary saves and `index.json` live in **`save_data/`** at the repo root (see `server/docker-compose.yml`). The image can also run **Dropbox sync in the background** - set `GBASYNC_DROPBOX_MODE` and related vars in the same root `.env` (see `USER_GUIDE.md` §1). For upload-driven sync bursts, use `GBASYNC_DROPBOX_SYNC_ON_UPLOAD=true` with `GBASYNC_DROPBOX_SYNC_DEBOUNCE_SECONDS=10` and keep `GBASYNC_DROPBOX_INTERVAL_SECONDS` long as fallback.

**Delta + `server_delta`:** The Dropbox API bridge only writes into Harmony `GameSave-*` slots that **already exist** for each ROM. Your 3DS upload goes to `/save/pokemon-fire-bpre`, but that blob is pushed to Dropbox **only** if Delta has a matching row—typically **Pokémon: Fire Red Version** (retail) with a save file. If your Dropbox folder only has BPRE *hacks* (e.g. Unbound, RedRocket) and no retail Fire Red entry, Fire Red’s server save will not appear in Delta until you open the retail game once in Delta (or use separate server ids per hack). Watch container logs for `[server_delta] note: server has /save/pokemon-fire-bpre but **no Delta GameSave row**`.

Health check:

```bash
curl http://127.0.0.1:8080/health
```

### 2) Delta / desktop bridge (optional if not using Docker Dropbox mode)

```bash
cd bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp config.example.json config.json
python bridge.py --config config.json --once
```

### 3) Install console clients

Generate release artifacts:

```bash
./scripts/release-server.sh v0.1.3
./scripts/release-bridge.sh v0.1.3
./scripts/release-switch.sh v0.1.3
./scripts/release-3ds.sh v0.1.3
```

Then use:

- `dist/switch/.../INSTALL.txt`
- `dist/3ds/.../INSTALL.txt`

## Documentation

- `USER_GUIDE.md`: full setup and usage guide
- `TODO.md`: recently shipped items vs backlog
- `RELEASE.md`: packaging and release workflow
- `dist/README.md`: dist artifact glossary and install pointers
- `server/README.md`: server API and run details
- `docs/HARDWARE_VALIDATION_CHECKLIST.md`: real-device validation checklist
- `PLAN.md`: implementation checklist and progress tracking
- `IDEA.md`: project concept notes

## Smoke Test Harness

Run a local server+bridge smoke test:

```bash
./scripts/smoke-sync.sh
```

This verifies upload and download flow in an isolated temp directory.

## Key Behaviors

- **A / full sync (Auto):** Uses **SHA-256** plus **`.gbasync-baseline`** on the device (legacy **`.savesync-baseline`** is still read/written for compatibility). First time a `game_id` has no baseline row, Auto logs **SKIP (no baseline yet)** until you run **upload-only** or **download-only** once for that game. **Switch** asks for confirm (**A** continue, **B** back; **+** is not cancel on that screen). After any sync run, **Switch** shows **A** = main menu / **+** = exit; **3DS** shows **A** = main menu / **START** = exit app.
- **X / upload-only** and **Y / download-only:** checklist UI (**ALL SAVES** or per-game); force upload or download for picks (Switch: **+** to run, **B** back; 3DS: **START** / **R** / **X** or **Y** to run, **B** back).
- **Conflicts:** when local and server both diverged from baseline, a **Conflict** prompt (**X** / **Y** / **B**) on Switch and 3DS.
- **`GET /saves`** on the server comes from **`index.json`**, not from re-scanning the save folder; **`DELETE /save/{game_id}`** cleans index + blob. See `server/README.md` and `USER_GUIDE.md`.
- ROM-header-based **`game_id`** when `[rom]` is configured (bridge, Switch, 3DS); otherwise normalized save stem.
- Atomic local writes; optional server version history; API-key auth.

## Delta Dropbox Sync Notes

- `delta_dropbox_api_sync.py` uploads **blobs first**, then `GameSave-*` JSON sidecars, and aligns sidecar `files[0].versionIdentifier` to the uploaded blob's Dropbox `rev`.
- This keeps Harmony metadata and attachment revisions in sync and avoids common Delta download/call errors caused by stale revision pointers.
- In two-way mode (`GBASYNC_SERVER_DELTA_ONE_WAY=false`), Delta may still show a **one-time conflict chooser** after prior divergent history; choose the side you want once, then normal sync should stabilize.
- Recommended two-way guardrails in `.env`:
  - `GBASYNC_DROPBOX_INTERVAL_SECONDS=120`
  - `GBASYNC_SERVER_DELTA_MIN_DELTA_WIN_SECONDS=900`
  - `GBASYNC_SERVER_DELTA_RECENT_SERVER_PROTECT_SECONDS=3600`

## MVP Limitations

- Switch/3DS clients use plain **`http://`** (no TLS in this path yet)
- Console sync is foreground/manual (not a background service)

## Toolchain Requirements (for building clients)

- Python 3.11+ (server/bridge)
- Docker Desktop (server container workflow)
- devkitPro with:
  - `devkitA64` + `libnx` (Switch)
  - `devkitARM` + `libctru` (3DS)
- 3DS packaging tools for `./scripts/release-3ds.sh`:
  - `makerom` (build `.cia`)
  - `bannertool` (build icon/banner assets)
  - `sips` on macOS (resizes banner image in current Makefile)

## Contributing / next work

See **`TODO.md`** for backlog. Broad themes: HTTPS on consoles, optional confirms for X/Y modes, deeper tests, and optional background sync.

## License

This project is licensed under the `GBAsync Non-Commercial License 1.0`.
See `LICENSE` for full terms.
