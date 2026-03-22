# GBAsync — TODO / backlog

**Latest release notes:** `docs/RELEASE_NOTES_v0.1.8.md` (prior: `docs/RELEASE_NOTES_v0.1.7.md`)

## Recently completed (sync UX + server)

- **Save history (server + admin + consoles):** **`HISTORY_MAX_VERSIONS_PER_GAME`** retention with **keep pins** — `pins.json` per game; trim removes **unpinned** oldest files first so pinned backups are not counted toward the cap. **`GET /save/{game_id}/history`**, **`POST /save/{game_id}/restore`**, **`PATCH .../history/revision/keep`**. Admin: **Display name** vs **label save** (per-history revision), history sheet with **Keep** + restore. **Switch + 3DS:** save viewer **A** → history list; **R** keep/unkeep; **`[KEEP]`** prefix; server restore then user downloads locally; **save viewer** shows **`display_name`** instead of **`game_id`** when set; blank lines between rows; **3DS** history uses heap-backed rows (stack overflow fix).
- **Auto (full) sync** on Switch and 3DS driven by **SHA-256** and per-device **`.gbasync-baseline`** (legacy **`.savesync-baseline`** still supported; not unreliable SD mtimes for merge decisions).
- **Conflict screens** on both consoles: **X** push local (force), **Y** pull server, **B** skip.
- **HTTP hardening** on console clients (e.g. chunked decode, `Accept-Encoding: identity`, tolerant server JSON).
- **Switch:** **Post-sync** screen: **A** main menu, **+** exit app.
- **3DS:** **Post-sync** screen: **A** main menu, **START** exit app (skips the extra “Press START” exit prompt when you quit from there).
- **Server:** **`DELETE /save/{game_id}`** removes metadata from **`index.json`** and deletes **`{game_id}.sav`** under `SAVE_ROOT` (cleanup after tests / curl). **`GET /saves`** is index-driven, not a raw directory listing.
- **Server identity layer:** optional `rom_sha1` on upload + canonical alias routing (`alias -> canonical`, tombstones for merged ids), with backward-compatible index format.
- **Bridge/Delta mapping hardening:** deterministic slot mapping order (slot-map, ROM SHA, title/filename, safe fallbacks), plus auto-bootstrap of `delta_slot_map`.
- **Dropbox Delta sync regression fix:** retail titles like Fire Red can map to friendly server ids (e.g. `firered`) via normalized title/`filename_hint` matching.
- **Release flow:** built and packaged **`v0.1.6`** (consoles + notes); earlier **`v0.1.5`** server/bridge/3DS/Switch drops and mapping docs.
- **Server data location:** configurable via repo-root `.env` (`SAVE_ROOT`, `INDEX_PATH`, `HISTORY_ROOT`); default tree is `save_data/` at repo root (see `server/README.md`).

### v0.1.6 — console clients (Switch + 3DS)

- **Auto: plan → preview → apply:** After **read-only** work (local scan, `GET /saves`, baseline load), classify each merged `game_id` (OK / upload / download / skip / conflict / locked) **without** applying changes. User confirms on the **preview**, then **apply** runs the existing PUT/GET / conflict / baseline paths. Replaces the old generic “confirm full sync” step on both platforms.
- **`.gbasync-status`:** Small file next to **`.gbasync-baseline`** (active save dir on 3DS); main menu prints last sync time, server reachability, Dropbox last result, optional short error (updated after sync and Dropbox attempts).
- **Per-device locks:** **`[sync] locked_ids=`** in `config.ini` (comma-separated `game_id`s). Locked ids are **skipped** on Auto (with log). **Save viewer** (main menu **R**) lists the union of local + server ids; **R** toggles lock for the highlighted row and **rewrites `config.ini`**.
- **3DS specifics:** **`save_locked_ids_to_ini_3ds`** uses **heap-allocated** INI line buffers (large rewrites no longer blow the default **~16 KiB** thread stack). **AUTO** path refactored to the same **plan → preview → apply** shape as Switch.
- **Docs / README:** `docs/USER_GUIDE.md` and **`switch-client/README.md` / `3ds-client/README.md`** user-flow sections describe preview, status, locks, and controls.

### v0.1.6 — follow-up parity + UI polish

- **“Already Up To Date”:** Triggers when **every plan row is OK** (`nk == plan size`), **not** when “no upload/download/skip/conflict counts” — the latter wrongly matched **all locked** games too.
- **Preview screen:** Lists **non-OK** work only (OK rows hidden); header counts **omit OK**; **lock toggle removed from preview** — use **Save viewer** to change `locked_ids`; **A/B** confirm copy and **dirty** redraw (no full-screen clear every frame).
- **Apply:** Omits per-game **`game_id: OK`** log lines when local and remote already match.
- **Switch main menu:** **Two-column** key hints aligned with the 3DS layout.
- **Flicker / static screens:** **Save viewer** uses **dirty** redraw on both platforms; **Switch** upload-only / download-only pickers use **dirty** redraw; HTTP error / empty-list screens **draw once**; **3DS** save viewer errors **`consoleClear`** before the message so the menu doesn’t show through.

### Admin web UI (server)

- **Shipped** static admin UI in **`admin-web/static/`** (no bundler): **Dashboard** snapshot, **Saves** table (hash preview, conflict flag, links to resolve), **Conflicts**, read-only **Index routing** (aliases, `rom_sha1`, tombstones), optional **Slot map** JSON when `GBASYNC_SLOT_MAP_PATH` is set, **Actions** (Dropbox sync-once, conflict resolve, delete save with typed confirmation).
- **Served** by FastAPI: **`/admin/ui/`** (static), **`/admin`** redirect, browser **`GET /`** can redirect to the UI; see **`admin-web/README.md`**.
- **Auth:** enabled when **`GBASYNC_ADMIN_PASSWORD`** is set; **HttpOnly** session cookie or **`X-API-Key`** for scripts; details in **`admin-web/README.md`** and **`server/app/admin.py`**.

### v0.1.6+ — polish & performance (consoles)

- **Menu copy:** Main menu **Auto sync** (not “full sync”) on Switch and 3DS.
- **Switch main menu status:** **Last sync** / **Server** / **Dropbox** on **separate lines** (matches 3DS); blank line between **Save dir** and the status block.
- **Switch exit:** Single **`+`** from the main menu exits the app (no second “Press +” screen); config-error path still shows **Press +** once.
- **Switch Auto:** **`Scanning local saves...`** prints (with `consoleUpdate`) before the local scan. **Post-sync log** omits **`Local saves:`** / **`Remote saves:`** (preview already shows the plan); **blank line** after confirm, then per-game **`UPLOADED` / `DOWNLOADED` / …** like 3DS. **X/Y** modes still print **`Local saves:`** / **`Remote saves:`** before results.
- **Dropbox on Switch:** **`Dropbox sync now...`** before the sync-once request (same idea as 3DS).
- **Upload / download pickers:** Rows list **`game_id` only** (no filename / `filename_hint` in parentheses).
- **ROM `game_id` resolution:** When **`[rom]`** paths are set, Switch and 3DS read only the **first 512 bytes** of a matching ROM for the GBA header (not the full ROM per save) — large speedup when many saves share `rom_dir`.
- **Save viewer:** Control hints are **one action per line** (move, then lock toggle, then back); not “move + R” on one row.

## Backlog / ideas

- Background or scheduled sync on consoles (today: open app, sync, foreground only).
- Optional **confirm** for **upload-only** / **download-only** (Auto sync already confirms via preview where implemented).
- **HTTPS** for Switch/3DS HTTP clients.
- **Test pass:** validate client `config.ini` with explicit `rom_dir=...` and `rom_extension=...` on both 3DS/Switch (good path + missing ROM fallback path), and confirm server canonical/alias behavior stays correct.

## v0.1.6 UX roadmap (prioritized)

### Quick wins (high impact, low-to-medium effort)

_(Done — see **Recently completed**.)_

### Medium effort

- **Actionable conflict details:** show both timestamps/source labels on conflict choices (push local / pull server / skip).
- **Server auto-discovery on LAN (optional):** mDNS/Bonjour or quick subnet probe to avoid manual IP entry.

### Longer-term

- **Profiles / namespaces:** multiple API keys or save namespaces for shared households/devices.
- **History UX polish:** richer on-device conflict of “local vs restored server snapshot” (today: restore on server; user runs download/Auto to align the handheld).

### Maybe do
- **One-tap diagnostics export:** write redacted logs + config summary + app/server version to a text file for support.
- **First-run setup wizard (3DS/Switch):** guided URL/API key entry, connection test, and initial sync test to reduce config friction.

## Old scratch notes (superseded)

Earlier lines like “automatic sync”, “are you sure”, and “persistent storage” referred to MVP gaps that baseline Auto, preview/confirm, and the server index largely cover; remaining gaps are listed in **Backlog** above.
