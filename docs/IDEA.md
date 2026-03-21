# GBAsync

Cross-platform GBA save synchronization system across:

- Homebrew Nintendo 3DS
- Homebrew Nintendo Switch
- Delta emulator on iOS (via desktop bridge)

## Step-by-step plan

### Phase 0 - Foundation

1. Define repository structure and shared concepts (`game_id`, metadata schema, conflict policy).
2. Establish local development workflow and deployment baseline (Docker for server).
3. Write top-level documentation and milestones.

### Phase 1 - Server + Delta bridge (MVP shipped)

1. Build FastAPI sync server with API-key auth.
2. Implement binary save upload/download plus metadata index.
3. Add version-history backups and conflict detection; **`DELETE /save/{game_id}`** for cleanup.
4. Build desktop Delta bridge with:
   - one-shot mode (`--once`)
   - watch mode (`--watch`)
   - periodic pull
5. Add tests for conflict handling and sync decision logic.

### Phase 2 - Switch homebrew client (MVP shipped)

1. Scaffold libnx project and HTTP client module.
2. Parse `config.ini` from `sdmc:/switch/gba-sync/config.ini`.
3. Implement local scan + pull/push logic + safe writes; baseline Auto + conflict UI.
4. Add text UI: per-line logs, confirm + post-sync navigation.

### Phase 3 - 3DS homebrew client (MVP shipped)

1. Scaffold devkitARM/libctru project.
2. Parse `config.ini` from `sdmc:/3ds/gba-sync/config.ini`.
3. Implement memory-conscious sync logic and retry handling; baseline Auto + conflict UI.
4. Add bottom-screen sync log, confirm + post-sync navigation, robust HTTP.

### Phase 4 - Hardening (largely shipped; polish remains)

1. Conflict resolution: **`POST /resolve/{game_id}`** + on-console conflict UI.
2. Richer telemetry/log rotation — still open.
3. Server tests + smoke scripts; broader edge-case matrix — ongoing.
4. Packaging/release scripts for each target — shipped (`RELEASE.md`, `dist/`).
5. Server **`DELETE /save/{game_id}`**; console HTTP hardening; baseline Auto sync.

## Current status

- [x] Plan and repository scaffold
- [x] Sync server (MVP)
- [x] Delta bridge (MVP)
- [x] Switch client (MVP implementation)
- [x] 3DS client (MVP implementation)

## Release packaging

Use `RELEASE.md` for reproducible packaging across all targets.
Use `USER_GUIDE.md` for end-user setup and operation.
Use `dist/README.md` for artifact-by-artifact install guidance.

Quick commands:

```bash
./scripts/release-server.sh v0.1.3
./scripts/release-bridge.sh v0.1.3
./scripts/release-switch.sh v0.1.3
./scripts/release-3ds.sh v0.1.3
```

## Shared concepts

### `game_id`

`game_id` is a stable key for a save slot:

- **Preferred (when `[rom]` / bridge ROM paths are set):** GBA ROM header (`title + game code`).
- **Fallback:** normalized save filename stem.

### Conflict policy

- **Consoles (Auto):** merge decisions use **SHA-256** + **`.gbasync-baseline`** (legacy **`.savesync-baseline`** still supported), not SD mtime.
- **Server / PUT path:** timestamp ordering, conflict flag when equal timestamp + different hash; optional **`POST /resolve/{game_id}`**.

### Safety

- Hash validation on upload.
- Temporary-file write + atomic rename on download.
- Version-history backup before replacement.

## Quick start

## Toolchain prerequisites

- Python 3.11+ for server and bridge
- devkitPro + `libnx` for `switch-client`
- devkitARM + `libctru` for `3ds-client`

### Server

```bash
cp .env.example .env   # repository root (once)
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --reload --host 0.0.0.0 --port 8080
```

### Bridge (Delta desktop sync agent)

```bash
cd bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python bridge.py --config config.example.json --once
```

## Next command sequence

1. Start server.
2. Run bridge in `--once` mode to validate sync.
3. Switch to `--watch` mode for continuous desktop syncing.
