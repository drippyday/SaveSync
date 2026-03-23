# GBAsync v1.0.0 — First major release

**v1.0.0** is the first **numbered major release** of GBAsync: a **self-hosted** system that keeps **portable save files** (`.sav` and related) aligned across a **central server**, **Nintendo Switch** and **Nintendo 3DS** homebrew clients, and—optionally—**Delta** on iOS via **Dropbox**-backed **Harmony** file sync. This document summarizes **everything the project does today** as shipped in this tag (see also **`docs/USER_GUIDE.md`** for setup and **`LICENSE`** for terms).

---

## Product overview

- **One server** holds the authoritative copy of each save **per `game_id`**, plus metadata (timestamps, SHA-256, optional display names, optional ROM SHA-1 for routing).
- **Console clients** use **plain HTTP** + **API key** to list saves, upload, download, and run **Auto** sync (plan → preview → apply) with **SHA-256** baselines, **conflict** detection, and optional **per-game locks**.
- **Optional version history** on the server: each replacement can archive the previous blob; retention is capped per game with **keep** pins for favorites.
- **Optional web admin** in the browser: dashboard, save management, history/restore, **upload** from disk, **row order** for `GET /saves`, index routing tools, Dropbox actions.
- **Optional Dropbox integration**: **flat** `.sav` folders, **Harmony** (Delta Emulator) trees via **desktop sync** or **Dropbox HTTP API**, with merge modes documented in **`bridge/`** and **`docs/USER_GUIDE.md`**.

Sync on consoles is **foreground** (you open the app and run a sync). There is **no TLS** on the device HTTP path in this release.

---

## Server (`server/`)

### Runtime

- **FastAPI** + **uvicorn**; health at **`GET /health`**. OpenAPI app version **`1.0.0`** (matches this tag).
- Config via **environment** (see repo-root **`.env.example`**): `API_KEY`, `SAVE_ROOT`, `INDEX_PATH`, `HISTORY_ROOT`, optional history caps, admin password, Dropbox mode, etc.
- **`gbasync.*` application logging** to stderr so admin uploads and similar **INFO** lines appear in Docker/process logs (not only uvicorn access logs).
- **Atomic** writes for save blobs; **index-backed** listing (`GET /saves` is **not** a raw directory scan).

### Core HTTP API (API key: `X-API-Key` or compatible header)

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/` | JSON service descriptor, or **browser** redirect to **`/admin/ui/`** when `Accept` prefers HTML |
| `GET` | `/health` | Liveness |
| `GET` | `/saves` | Ordered list of saves with metadata (`display_name`, `sha256`, `size_bytes`, conflict, timestamps, `list_order`, …) |
| `GET` | `/conflicts` | Subset of saves in conflict |
| `GET` | `/save/{game_id}/meta` | `SaveMeta` JSON |
| `GET` | `/save/{game_id}` | Raw save bytes (`application/octet-stream`) |
| `PUT` | `/save/{game_id}` | Upload save; query: `last_modified_utc`, `sha256`, `size_bytes`, optional `rom_sha1`, `filename_hint`, `platform_source`, optional `client_clock_utc`, optional `force` |
| `DELETE` | `/save/{game_id}` | Remove index row and delete blob on disk |
| `POST` | `/resolve/{game_id}` | Clear conflict flag for that game |
| `POST` | `/dropbox/sync-once` | Run one Dropbox bridge pass in-process (see env) |
| `GET` | `/save/{game_id}/history` | List history revisions (labels, keep state, display strings) |
| `POST` | `/save/{game_id}/restore` | Restore active save from a named history file |
| `PATCH` | `/save/{game_id}/meta` | Set/clear main-row **`display_name`** |
| `PATCH` | `/save/{game_id}/history/revision` | Per-revision **label** (`display_name` for a backup) |
| `PATCH` | `/save/{game_id}/history/revision/keep` | Pin/unpin a history file (`keep`) |
| `POST` | `/debug/client-clock` | Optional client wall-clock report (logging when enabled) |

### Identity & index features

- **`game_id`** strings as stable keys; optional **`rom_sha1`** on uploads for canonical routing.
- **Aliases** (`alias` → canonical `game_id`), **`rom_sha1`** map, **tombstones** for merged ids—persisted in the index and editable from the **admin** UI.
- **Conflict** flag when equal client timestamp claims but different payload (**SHA-256**).
- **`force`** on PUT to accept uploads even when timestamp ordering would otherwise skip (used by clients and admin upload).

### Version history

- When enabled, replacing a save **backs up** the prior blob under **`HISTORY_ROOT/{game_id}/`** with timestamped filenames.
- **`HISTORY_MAX_VERSIONS_PER_GAME`**: trim **unpinned** oldest first; **`pins.json`** records **keep** pins.
- Per-revision **labels** stored alongside history; **keep** survives trimming priority.

### Docker & Dropbox sidecar

- **`docker-compose`** can mount host **`save_data`** (or custom paths) and ROMs for **`GBASYNC_ROM_DIRS`**.
- If **`GBASYNC_DROPBOX_MODE`** is `plain` or `delta_api`, the entrypoint runs **`write_bridge_config`**, **uvicorn**, and a **sidecar** that periodically runs the matching bridge script.
- **`GBASYNC_DROPBOX_SYNC_ON_UPLOAD`**: debounced **bridge** run after a successful PUT that applied new bytes.
- Bridge subprocess **does not capture** stdout—**`[dropbox]` / `[server_delta]`** lines appear in the same logs as the server.

---

## Admin web UI (`admin-web/static/`)

Served at **`/admin/ui/`** when **`GBASYNC_ADMIN_PASSWORD`** is set. Auth: **HttpOnly** session cookie after login, or **`X-API-Key`** for scripts (same key as the main API), optional **`GBASYNC_ADMIN_SECRET`** for cookie HMAC.

| Area | Behavior |
|------|----------|
| **Dashboard** | Save/conflict counts, Dropbox mode, paths; optional **max history versions** setting |
| **Saves** | Table + mobile cards; **Download**, **Upload** (replace server save from `.sav`; **sha256** optional when browser has no Web Crypto—server hashes body), **History**, **Display name**; **drag** row reorder when search is empty, then **Save row order** → **`PUT /admin/api/save-order`** |
| **Conflicts** | List + link to resolve |
| **Index routing** | View/edit **aliases**, **`rom_sha1`**, **tombstones**; save to server |
| **Slot map** | Shows JSON when **`GBASYNC_SLOT_MAP_PATH`** is readable |
| **Actions** | **Dropbox sync once**, resolve conflict by `game_id`, **delete** save (typed confirmation) |

**Upload success** uses a modal (or **`alert`** fallback) and a fixed **status** strip for errors/progress.

**Admin HTTP API** (prefix **`/admin/api/`**, same auth): **`/me`**, **`/login`**, **`/logout`**, **`/dashboard`**, **`/settings`** (PATCH), **`/index-state`**, **`/index-routing`** (PUT), **`/slot-map`**, **`/saves`**, **`/conflicts`**, **`/save/{game_id}/download`**, **`/save/{game_id}`** (PUT, optional **`sha256`** query), **`/save/{game_id}/history`**, restore, revision label/keep, **`/save-order`** (PUT), **`/save/{game_id}/meta`** (PATCH), **`/dropbox/sync-once`**, **`/resolve/{game_id}`**, **`DELETE /save/{game_id}`**.

---

## Nintendo Switch client (`switch-client/`)

- **`sdmc:/switch/gba-sync/config.ini`**: server URL, API key, save roots, ROM dirs, optional **`locked_ids`**, optional **`sync_nds_saves`**, **`skip_save_patterns`**, etc.
- **Multi-root**: GBA / NDS / GB·GBC save directories with matching ROM trees; **legacy** single **`save_dir`**.
- **`game_id`**: GBA ROM header, **NDS** cartridge header, GB/GBC header—or stem fallback when ROM missing (aligned with bridge logic).
- **HTTP**: `GET /saves`, `GET`/`PUT /save/{id}`, metadata/history endpoints as needed; resilient parsing (chunked bodies, encoding).
- **Auto sync**: build **plan** → **preview** (non-OK rows only) → **apply**; **SHA-256** baselines in **`.gbasync-baseline`** per root (legacy **`.savesync-baseline`**).
- **Conflicts**: on-console resolution (**X** push local, **Y** pull server, **B** skip).
- **Save viewer** (main **R**): local∪server list, **display_name** when set, **R** lock, **A** history/restore, **R** in history = keep.
- **Upload (X)** / **download (Y)** pickers; **+** exit; post-sync **Y: reboot** in defined cases.
- **`.gbasync-status`**: last sync / server / Dropbox line on main menu.

---

## Nintendo 3DS client (`3ds-client/`)

- **`sdmc:/3ds/gba-sync/config.ini`**: same general shape as Switch; **`mode=normal`** vs **`mode=vc`** (VC save dir).
- **Multi-root** GBA/NDS/GB with **`gba_save_dir`**, **`nds_save_dir`**, **`gb_save_dir`** (when any set, legacy **`save_dir`** ignored in normal mode).
- Same **Auto** / **preview** / **baseline** / **conflict** / **save viewer** / **history** / **keep** model as Switch (controls differ—**START** / **A** / **B** patterns per README).
- **`.gbasync-status`** beside baseline in the active save folder.

---

## Bridge & Dropbox (`bridge/`)

### `bridge.py` (local folder ↔ server)

- Watches a **local** folder of plain **`*.sav`** files; **`--once`**, **`--watch`**, **`--dry-run`**.
- Resolves **`game_id`** via ROM maps / `rom_dirs` / filename stems (**`bridge/game_id.py`** shared with other tools).

### `dropbox_bridge.py`

- Syncs a **Dropbox API** path of **flat** `*.sav` files with the server (not Harmony layout). **`--once`** or **`--watch`**.

### `delta_folder_server_sync.py`

- Merges **server** ↔ **local Delta Emulator folder** on disk (`triple` or `server_delta`); rewrites **Harmony** `GameSave-*` blobs/metadata via **`apply_bytes_to_delta`**.
- **Slot map**, ROM SHA-1 matching, title/filename heuristics, **server_delta_one_way** and two-way guardrails.

### `delta_dropbox_api_sync.py`

- Same merge idea when the Delta tree exists **only in Dropbox**: download → merge → upload (blob order + **`versionIdentifier`** alignment for fewer Delta conflicts).

### `delta_dropbox_sav.py`

- CLI to **list** / **export** Harmony saves to `.sav` and **import** `.sav` back into a Delta tree with backups.

### Cross-cutting

- **mGBA** often uploads **131088** bytes; Delta slots often expect **131072**—when writing into Harmony, the bridge **trims** the 16-byte footer in that case only (`[delta-apply] trim …` in logs).
- **NDS** + **GB/GBC** ROM identity in **`game_id`** matches consoles (cartridge headers, not mistaken GBA parsing for `.gb`/`.gbc`).

---

## Configuration highlights (env)

Not exhaustive—see **`.env.example`** and **`docs/USER_GUIDE.md`**.

- **`API_KEY`**, **`SAVE_ROOT`**, **`INDEX_PATH`**, **`HISTORY_ROOT`**, **`ENABLE_VERSION_HISTORY`**, **`HISTORY_MAX_VERSIONS_PER_GAME`**
- **`GBASYNC_ADMIN_PASSWORD`**, optional **`GBASYNC_ADMIN_SECRET`**, **`GBASYNC_SLOT_MAP_PATH`**
- **`GBASYNC_DROPBOX_MODE`**: `off` | `plain` | `delta_api` (legacy **`SAVESYNC_*`** names still read by **`write_bridge_config`**)
- **`GBASYNC_PUBLIC_SERVER_URL`**, **`GBASYNC_ROM_DIRS`**, **`DROPBOX_*`**, **`DROPBOX_REMOTE_DELTA_FOLDER`**, **`GBASYNC_SERVER_DELTA_ONE_WAY`**, timing/debounce keys

---

## Tests & tooling

- **pytest** suite under **`server/tests/`** (API roundtrips, admin, ordering, conflicts, etc.).
- **`./scripts/smoke-sync.sh`** — quick multi-component smoke check (see script).
- **Release scripts**: **`docs/RELEASE.md`** — `./scripts/release-server.sh`, `release-bridge.sh`, `release-switch.sh`, `release-3ds.sh`.

---

## Known limitations (v1.0.0)

- Console ↔ server is **HTTP without TLS** on the device path.
- Sync is **manual/foreground** on Switch and 3DS (open app, run sync).
- **Delta / Harmony** file editing is **best-effort** compatibility with Delta’s cloud format—not an official Delta API.
- Large Dropbox trees can be **bandwidth-heavy** on API mode; see **`docs/DROPBOX_SYNC_PERFORMANCE_PLAN.md`** for future ideas.

---

## Upgrade from prior tags (e.g. v0.1.8)

- **Server**: rebuild/load the new Docker image; keep **`save_data/`** (or your custom paths) and **`.env`**; run against the same index unless you intentionally migrate.
- **Consoles**: install new **`.nro`** / **`.3dsx`** (or `.cia`); **`config.ini`** is generally backward compatible—add keys only if you use new multi-root or NDS/GB features.
- **Bridge**: replace the zip; refresh **`config`** JSON if you changed env-driven bridge behavior.

---

## Build artifacts (example commands)

```bash
./scripts/release-server.sh v1.0.0
./scripts/release-bridge.sh v1.0.0
./scripts/release-switch.sh v1.0.0
./scripts/release-3ds.sh v1.0.0
```

Typical outputs (exact names match the tag):

- **`dist/server/gbasync-server-v1.0.0.zip`** — contains **`gbasync-server-v1.0.0/`** with **`gbasync-server-v1.0.0.tar`** (Docker image, includes **`admin-web/`**), **`docker-compose.yml`**, **`.env`**, **`README.md`**.
- **`dist/bridge/gbasync-bridge-v1.0.0.zip`**
- **`dist/switch/gbasync-switch-v1.0.0.zip`** (and unpacked folder) — `gbasync.nro`, `.nacp`, **`README.md`**, **`gba-sync/config.ini`**, **`gba-sync/README.md`**
- **`dist/3ds/gbasync-3ds-v1.0.0.zip`** — `gbasync.3dsx`, optional `.cia`, same **`README.md`** / **`gba-sync/`** layout

Set **`APP_VERSION`** in **`switch-client/Makefile`** to match the release tag so **`.nacp`** metadata matches the package.

---

## License

**GBAsync Non-Commercial License 1.0** — see **`LICENSE`**.

---

## See also

| Doc | Purpose |
|-----|---------|
| **`README.md`** | Project introduction and doc index |
| **`docs/USER_GUIDE.md`** | End-to-end setup and operations |
| **`docs/RELEASE.md`** | Maintainer packaging |
| **`server/README.md`** | API + `save_data/` layout |
| **`admin-web/README.md`** | Admin UI auth and behavior |
| **`bridge/README.md`** | Bridge scripts and Delta mapping |
| **`dist/README.md`** | What’s inside release zips |

Prior per-tag notes (**v0.1.5**–**v0.1.8**) remain in **`docs/RELEASE_NOTES_v0.1.*.md`** for incremental history.
