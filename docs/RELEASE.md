# GBAsync Release Guide

This document defines packaging and release steps for each component.

For end-user setup and operation instructions, see `USER_GUIDE.md`.

## 1) Server release

Produces:

- Docker image tarball
- `.env.example` (repository root; server + Dropbox template)
- `docker-compose.yml`

Command:

```bash
./scripts/release-server.sh v0.1.3
```

Artifacts:

- `dist/server/gbasync-server-v0.1.3.tar`
- `dist/server/.env.example`
- `dist/server/docker-compose.yml`

## 2) Bridge release

Produces:

- Zip package with bridge runtime files and helper run scripts.

Command:

```bash
./scripts/release-bridge.sh v0.1.3
```

Artifact:

- `dist/bridge/gbasync-bridge-v0.1.3.zip`

## 3) Switch release

Prerequisites:

- `DEVKITPRO` set
- `libnx` toolchain installed
- `nacptool` and `elf2nro` available under `$(DEVKITPRO)/tools/bin`

Command:

```bash
./scripts/release-switch.sh v0.1.3
```

Artifacts directory:

- `dist/switch/gbasync-switch-v0.1.3/`

Expected files (depending on build):

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
./scripts/release-3ds.sh v0.1.3
```

Artifacts directory:

- `dist/3ds/gbasync-3ds-v0.1.3/`

Expected files (depending on build):

- `*.3dsx`
- `*.cia` (if configured)
- `*.smdh`

## 5) Suggested release checklist

1. `cd server && source .venv/bin/activate && pytest -q`
2. Manual sync smoke test:
   - start server
   - run bridge `--once`
   - verify `GET /saves`
3. Build Switch client
4. Build 3DS client
5. Generate all `dist/` artifacts
6. Publish release notes with:
   - API changes (e.g. new **`DELETE /save/{game_id}`**)
   - console UX (baseline Auto, conflict UI, confirm + post-sync screens)
   - known limitations
   - upgrade steps

For Switch releases, confirm the embedded app version in `switch-client/Makefile` (`APP_VERSION`) matches the release tag so `gbasync.nacp` metadata is correct.

## Current limitations to mention in releases

- Console clients are **HTTP-only** (no TLS in this build path).
- **`game_id`:** ROM header when ROM paths are configured; else filename stem normalization.
- No background execution on Switch/3DS; sync is app-triggered.
- Server listing is **`index.json`**-backed; admins may need **`DELETE /save/{game_id}`** after test uploads.
