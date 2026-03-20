# Execution Plan

## Step 1 - Implement backend and desktop bridge (completed)

- [x] FastAPI server with save endpoints and API key auth
- [x] File-backed metadata index and binary persistence
- [x] Conflict detection + version-history backup
- [x] Delta bridge with `--once`, `--watch`, `--dry-run`
- [x] Server sync logic tests scaffolded

## Step 2 - Validate local workflow (in progress)

- [x] Resolve local pip SSL certificate issue (using trusted hosts for now)
- [x] Install dependencies in `server/.venv` and `bridge/.venv`
- [x] Run `pytest` in `server`
- [x] End-to-end test: create `.sav`, upload via bridge, verify pull/download

## Step 3 - Implement Switch client

- [x] Create libnx project files (`Makefile`, `source/*.cpp`)
- [x] Add config parser for `sdmc:/switch/gba-sync/config.ini`
- [x] Add HTTP sync client and local save scanner
- [x] Add text UI for sync results

## Step 4 - Implement 3DS client

- [x] Create devkitARM/libctru project files
- [x] Add 3DS config parser and save scanner
- [x] Add HTTP sync client with lightweight buffers (socket-based HTTP MVP)
- [x] Add bottom-screen logging UI

## Step 5 - Hardening

- [x] Add integration tests for conflict and interrupted-write-adjacent handling
- [x] Add optional conflict resolution endpoint
- [x] Add release packaging docs for each target
- [x] Console Auto sync: **`.savesync-baseline`** + SHA-256 merge policy (Switch + 3DS)
- [x] Conflict UX on consoles; HTTP client robustness (chunked / encoding / JSON tolerance)
- [x] Switch full-sync confirm + post-sync navigation; 3DS post-sync navigation
- [x] Server **`DELETE /save/{game_id}`** + documentation for **`index.json`** vs filesystem
