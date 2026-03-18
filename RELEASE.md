# SaveSync Release Guide

This document defines packaging and release steps for each component.

For end-user setup and operation instructions, see `USER_GUIDE.md`.

## 1) Server release

Produces:

- Docker image tarball
- `.env.example`
- `docker-compose.yml`

Command:

```bash
./scripts/release-server.sh v0.1.1
```

Artifacts:

- `dist/server/savesync-server-v0.1.1.tar`
- `dist/server/.env.example`
- `dist/server/docker-compose.yml`

## 2) Bridge release

Produces:

- Zip package with bridge runtime files and helper run scripts.

Command:

```bash
./scripts/release-bridge.sh v0.1.1
```

Artifact:

- `dist/bridge/savesync-bridge-v0.1.1.zip`

## 3) Switch release

Prerequisites:

- `DEVKITPRO` set
- `libnx` toolchain installed

Command:

```bash
./scripts/release-switch.sh v0.1.1
```

Artifacts directory:

- `dist/switch/savesync-switch-v0.1.1/`

Expected files (depending on build):

- `*.nro`
- `*.nacp`
- `*.elf`

## 4) 3DS release

Prerequisites:

- `DEVKITARM` set
- `libctru` toolchain installed

Command:

```bash
./scripts/release-3ds.sh v0.1.1
```

Artifacts directory:

- `dist/3ds/savesync-3ds-v0.1.1/`

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
   - API changes
   - known limitations
   - upgrade steps

## Current limitations to mention in releases

- 3DS client MVP is HTTP-only (no TLS in this build path).
- Game ID currently defaults to filename-derived normalization.
- No background execution on Switch/3DS; sync is app-triggered.
