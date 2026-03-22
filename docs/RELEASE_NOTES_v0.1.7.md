# GBAsync v0.1.7 — Admin save platform, client save viewer, labels & keep/lock

Release packaging **v0.1.7** bundles the **optional web admin** (dashboard + save management), **homebrew save viewer** flows on **Switch** and **3DS**, **per-game locks** and **history keep** pins, **display / revision labeling**, and a **Delta sync** fix for **mGBA-sized** GBA saves.

---

## Server & admin — dashboard and save management

With **`GBASYNC_ADMIN_PASSWORD`** set, the **browser admin UI** (`/admin/ui/`) is a practical **save management platform** alongside the API:

- **Dashboard** — snapshot of mode, paths, counts, and health-style context.
- **Saves** — indexed games with hash preview, size, conflict flags, links to resolve, **Display name** (friendly label for the main save row), and **History** per title.
- **History** — list server-side backup revisions, **label save** (per-revision note), **Keep** toggle (pinned revisions are not trimmed away first under **`HISTORY_MAX_VERSIONS_PER_GAME`**), and **restore** to a chosen revision.
- **Conflicts** — games currently in conflict; actions to resolve.
- **Index routing** — read-only view of aliases, ROM SHA1 map, tombstones.
- **Slot map** (optional) — when configured.
- **Actions** — Dropbox sync-once, conflict resolve, delete save (with confirmation).

Auth: **HttpOnly** session cookie or **`X-API-Key`** for scripts — see **`admin-web/README.md`**.

---

## Console clients — save viewer, lock, keep, and labels

### Save viewer (main menu **R**)

- Lists the union of **local + server** games; primary line shows **`display_name`** when the server has one, otherwise **`game_id`**.
- **R** toggles **lock** for the highlighted row (`locked_ids` in `config.ini`) so **Auto** skips that game.
- **A** opens **history / restore** for that row.
- **B** back.

### History screen (from save viewer **A**)

- Lists revisions (label + filename; 3DS avoids noisy timestamps on small screens).
- **R** toggles **keep** (pinned) for a revision (`PATCH .../history/revision/keep`); pinned rows show **`[KEEP]`**.
- **A** starts **restore**; confirm with **A** again on the prompt — changes the **server** copy only; pull to the device with **download** or **Auto** so local `.sav` matches.

### Lock vs keep (short)

| Concept | Where | Effect |
|--------|--------|--------|
| **Lock** | Save viewer **R** | Skip this **`game_id`** on **Auto** (per-device `locked_ids`). |
| **Keep** | History **R** | Pin a **history file** so retention trim drops **unpinned** backups first. |

### Game labeling

- **Server index:** **Display name** (admin or `PATCH /save/{game_id}/meta`) — shown in save viewer instead of raw **`game_id`** when set.
- **History:** **label save** (per backup revision) — separate from the main display name.

---

## Bridge / Docker — Delta (mGBA save size)

When pushing server bytes into **Delta’s Harmony** Dropbox layout, **mGBA** often uploads **131088** bytes (128 KiB + 16) while Delta’s slot expects **131072**. **`apply_bytes_to_delta`** trims the **trailing 16 bytes** only in that exact case, logs **`[delta-apply] trim …`**, then uploads. **Tests** and **`bridge/DELTA_DROPBOX_FORMAT.md`** / **`docs/USER_GUIDE.md`** describe the behavior.

---

## Docs & planning

- **`docs/USER_GUIDE.md`** — history, keep, pins, display names, mGBA/Delta size note.
- **`plans/gbasync-gba-and-nds-unified-sync.md`** — roadmap for future **GBA + NDS** unified config (not part of this tag’s feature set).

---

## Artifacts

```bash
./scripts/release-server.sh v0.1.7
./scripts/release-bridge.sh v0.1.7
./scripts/release-switch.sh v0.1.7
./scripts/release-3ds.sh v0.1.7
```

- **`dist/server/gbasync-server-v0.1.7.tar`** — Docker image (includes **`admin-web/`** static UI).
- **`dist/bridge/gbasync-bridge-v0.1.7.zip`**
- **`dist/switch/gbasync-switch-v0.1.7/`** — `gbasync.nro`, `.nacp` metadata **0.1.7**, `INSTALL.txt`, sample `gba-sync/config.ini`
- **`dist/3ds/gbasync-3ds-v0.1.7/`** — `gbasync.3dsx`, `gbasync.cia`, `INSTALL.txt`, sample `gba-sync/config.ini`

Switch **`.nacp`** version comes from **`switch-client/Makefile`** `APP_VERSION` (**0.1.7**).

---

## Upgrade

- **Docker:** load **`gbasync-server-v0.1.7.tar`**; set **`GBASYNC_ADMIN_PASSWORD`** (and API key) in `.env` to use the admin UI.
- **Consoles:** copy new **`gbasync.nro`** / **`gbasync.3dsx`** (or `.cia`); keep **`config.ini`**.
- **v0.1.6** users: safe upgrade; v0.1.7 adds the Delta trim and rolls up the admin + client features above into the same tagged build.

---

## See also

- Deeper **v0.1.6**-era changelog notes (preview, auto, polish): **`docs/RELEASE_NOTES_v0.1.6.md`**
