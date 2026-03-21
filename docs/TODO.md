# GBAsync — TODO / backlog

## Recently completed (sync UX + server)

- **Auto (full) sync** on Switch and 3DS driven by **SHA-256** and per-device **`.gbasync-baseline`** (legacy **`.savesync-baseline`** still supported; not unreliable SD mtimes for merge decisions).
- **Conflict screens** on both consoles: **X** push local (force), **Y** pull server, **B** skip.
- **HTTP hardening** on console clients (e.g. chunked decode, `Accept-Encoding: identity`, tolerant server JSON).
- **Switch:** full-sync **confirm** — **A** continue, **B** back to menu (**+** is *not* cancel on that screen, to avoid mistaken backs). **Post-sync** screen: **A** main menu, **+** exit app.
- **3DS:** **Post-sync** screen: **A** main menu, **START** exit app (skips the extra “Press START” exit prompt when you quit from there).
- **Server:** **`DELETE /save/{game_id}`** removes metadata from **`index.json`** and deletes **`{game_id}.sav`** under `SAVE_ROOT` (cleanup after tests / curl). **`GET /saves`** is index-driven, not a raw directory listing.

## Backlog / ideas

- Background or scheduled sync on consoles (today: open app, sync, foreground only).
- Optional **confirm** for **upload-only** / **download-only** (full sync already confirms where implemented).
- **HTTPS** for Switch/3DS HTTP clients.
- **Delta native cloud:** no Harmony SDK; optional **`dropbox_bridge.py`** for a plain `.sav` Dropbox folder (`bridge/DROPBOX.md`). For Delta’s **own** Dropbox export, **`delta_dropbox_sav.py`** exports/imports raw `GameSave-*-gameSave` blobs + JSON (`bridge/DELTA_DROPBOX_FORMAT.md`).
- Server: multi-user / richer admin if the single API key + file store stops being enough.

## Old scratch notes (superseded)

Earlier lines like “automatic sync”, “are you sure”, and “persistent storage” referred to MVP gaps that baseline Auto, full-sync confirm, and the server index largely cover; remaining gaps are listed in **Backlog** above.
