# SaveSync

Cross-platform GBA save synchronization across:

- Nintendo Switch (homebrew)
- Nintendo 3DS (homebrew)
- Delta on iOS (through a desktop bridge)

SaveSync lets you continue the same GBA game on different devices by syncing `.sav` files through a self-hosted server.

## Why This Project

Delta has cloud sync, but it is Delta-specific. SaveSync provides one shared sync system across multiple emulators/hardware targets with a consistent server-side source of truth.

## Architecture

SaveSync uses a client-server model:

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
cd server
cp .env.example .env
docker compose up -d
```

With Docker, binary saves and `index.json` live in **`save_data/`** at the repo root (see `server/docker-compose.yml`).

Health check:

```bash
curl http://127.0.0.1:8080/health
```

### 2) Run Delta bridge once

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
./scripts/release-server.sh v0.1.1
./scripts/release-bridge.sh v0.1.1
./scripts/release-switch.sh v0.1.1
./scripts/release-3ds.sh v0.1.1
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

- **A / full sync (Auto):** Uses **SHA-256** plus **`.savesync-baseline`** on the device (same idea on Switch and 3DS). First time a `game_id` has no baseline row, Auto logs **SKIP (no baseline yet)** until you run **upload-only** or **download-only** once for that game. **Switch** asks for confirm (**A** continue, **B** back; **+** is not cancel on that screen). After any sync run, **Switch** shows **A** = main menu / **+** = exit; **3DS** shows **A** = main menu / **START** = exit app.
- **X / upload-only** and **Y / download-only:** checklist UI (**ALL SAVES** or per-game); force upload or download for picks (Switch: **+** to run, **B** back; 3DS: **START** / **R** / **X** or **Y** to run, **B** back).
- **Conflicts:** when local and server both diverged from baseline, a **Conflict** prompt (**X** / **Y** / **B**) on Switch and 3DS.
- **`GET /saves`** on the server comes from **`index.json`**, not from re-scanning the save folder; **`DELETE /save/{game_id}`** cleans index + blob. See `server/README.md` and `USER_GUIDE.md`.
- ROM-header-based **`game_id`** when `[rom]` is configured (bridge, Switch, 3DS); otherwise normalized save stem.
- Atomic local writes; optional server version history; API-key auth.

## MVP Limitations

- Switch/3DS clients use plain **`http://`** (no TLS in this path yet)
- Console sync is foreground/manual (not a background service)

## Toolchain Requirements (for building clients)

- Python 3.11+ (server/bridge)
- Docker Desktop (server container workflow)
- devkitPro with:
  - `devkitA64` + `libnx` (Switch)
  - `devkitARM` + `libctru` (3DS)

## Contributing / next work

See **`TODO.md`** for backlog. Broad themes: HTTPS on consoles, optional confirms for X/Y modes, deeper tests, and optional background sync.

## License

This project is licensed under the `SaveSync Non-Commercial License 1.0`.
See `LICENSE` for full terms.
