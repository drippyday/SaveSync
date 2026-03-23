# GBAsync Release Guide

This document defines packaging and release steps for each component.

For end-user setup and operation instructions, see `docs/USER_GUIDE.md`.

## 1) Server release

Produces:

- **`dist/server/gbasync-server-<ver>.zip`** containing **`gbasync-server-<ver>/`** with:
  - Docker image tarball
  - `.env.example` / `.env` (template + copy)
  - `docker-compose.yml`
  - **`README.md`** (install / paths)

Command:

```bash
./scripts/release-server.sh v1.0.0
```

Artifacts (replace `v1.0.0` with your tag):

- `dist/server/gbasync-server-v1.0.0.zip`
- `dist/server/gbasync-server-v1.0.0/` (unpacked tree; same contents as the zip)

## 2) Bridge release

Produces:

- Zip package with bridge runtime files and helper run scripts.

Command:

```bash
./scripts/release-bridge.sh v1.0.0
```

Artifact:

- `dist/bridge/gbasync-bridge-v1.0.0.zip`

## 3) Switch release

Prerequisites:

- `DEVKITPRO` set
- `libnx` toolchain installed
- `nacptool` and `elf2nro` available under `$(DEVKITPRO)/tools/bin`

Command:

```bash
./scripts/release-switch.sh v1.0.0
```

Artifacts:

- `dist/switch/gbasync-switch-v1.0.0.zip`
- `dist/switch/gbasync-switch-v1.0.0/` — **`README.md`**, **`gba-sync/config.ini`**, **`gba-sync/README.md`**, plus:

- `*.nro`
- `*.nacp`
- `*.elf`

## 4) 3DS release

Prerequisites:

- `DEVKITARM` set
- `libctru` toolchain installed
- `smdhtool` and `3dsxtool` available under `$(DEVKITPRO)/tools/bin`
- `bannertool` installed (for icon/banner generation)
- `makerom` installed (for `.cia` packaging)
- `sips` available on macOS (used by current `3ds-client/Makefile` for banner resize)

Command:

```bash
./scripts/release-3ds.sh v1.0.0
```

Artifacts:

- `dist/3ds/gbasync-3ds-v1.0.0.zip`
- `dist/3ds/gbasync-3ds-v1.0.0/` — **`README.md`**, **`gba-sync/`** as above, plus:

- `*.3dsx`
- `*.cia` (if configured)
- `*.smdh`

## 5) Suggested release checklist

1. Bump the server’s reported version in **`server/app/main.py`** (`FastAPI(..., version=...)`) to match the tag.
2. `cd server && source .venv/bin/activate && pytest -q`
3. Manual sync smoke test:
   - start server
   - run bridge `--once`
   - verify `GET /saves`
4. Build Switch client
5. Build 3DS client
6. Generate all `dist/` artifacts
7. Add **`docs/RELEASE_NOTES_vX.Y.Z.md`** and publish with:
   - API changes (e.g. **`DELETE /save/{game_id}`**, save **history** / **restore** / **keep** if applicable)
   - console UX (baseline Auto, conflict UI, confirm + post-sync screens, save viewer + history)
   - known limitations
   - upgrade steps

For Switch releases, confirm the embedded app version in `switch-client/Makefile` (`APP_VERSION`) matches the release tag so `gbasync.nacp` metadata is correct.

## Current limitations to mention in releases

- Console clients are **HTTP-only** (no TLS in this build path).
- **`game_id`:** ROM header when ROM paths are configured; else filename stem normalization.
- No background execution on Switch/3DS; sync is app-triggered.
- Server listing is **`index.json`**-backed; admins may need **`DELETE /save/{game_id}`** after test uploads.
