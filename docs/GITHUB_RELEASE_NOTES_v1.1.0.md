# GBAsync v1.1.0

Fixes and small features across **admin web**, **server**, **Switch**, and **3DS**. **TLS/HTTPS for consoles is not in this release** (still plain HTTP on the LAN).

---

## Changes

### Admin web (`/admin/ui/`)

- **Save upload from disk (Mar 23, 2026)** — Row-targeted **Upload** always sends **`filename_hint`** as **`{game_id}.sav`** for the row you clicked, not the file’s name on disk. Importing e.g. `MyRom-1234.sav` into the correct game row no longer mis-keys metadata or leaves stray blobs when the filename doesn’t match the server **`game_id`**.
- **History on mobile (Mar 24, 2026)** — **Done** is a primary control in the **header** next to the title; the revision list scrolls underneath. Easier to close the sheet without scrolling past long histories.
- **Saves tab (mobile)** — Tighter spacing between the search field and **Save row order** (no flex “growth” gap).

### Server

- **`GET /saves`** — Optional pagination: **`limit`** and **`offset`** query parameters (1–10000). Response includes **`total`** (count before slicing). Clients that ignore unknown JSON fields stay compatible.
- **`PUT /save/{game_id}`** — Rejects bodies larger than **`GBASYNC_MAX_SAVE_UPLOAD_BYTES`** (default **4 MiB**). Returns **413** if exceeded.
- **Admin `GET /admin/api/saves`** — Includes **`total`** alongside **`saves`**.

### Switch client

- **Post-sync menu** — Removed **“Y: reboot now”** after sync; post-sync is **A: main menu** and **+: exit** only.
- **Auto — no baseline, both sides differ** — Treated as a **conflict**: same **X/Y/B** prompt as “both changed” instead of a **SKIP** line that told you to use upload/download once per game. On first launch with saves on both sides, you’ll see that prompt per game until baselines exist; the conflict screen uses **“No sync history on this device yet — choose which version to keep”** instead of the “both changed since last sync” wording (which still applies when a baseline exists but local and server both diverged from it).
- **Sync preview** — Rows show **`display_name`** when the server sent one; summary line drops the old **SKIP** column (no-baseline rows are **CONF** now). **+ (Plus): sync & exit** runs the apply phase then **exits the app** (no post-sync menu).
- **Main menu — “needs sync” badge** — **`N game(s) need sync (run Auto)`** when **N > 0**. The count is **cached** after the first successful compute; it is **recomputed** only after you return from **Auto sync**, **upload-only**, **download-only**, **Dropbox sync**, or **save viewer** (not on every menu redraw). Avoids a full local scan + **`GET /saves`** on every visit to the main menu.
- **HTTP** — **3 attempts**, **1s** backoff on failure (same pattern as 3DS).

### 3DS client

- **Auto — no baseline, both sides differ** — Same as Switch: **conflict** UI (**X/Y/B**) instead of **SKIP** + long help text. **First-time / no baseline** copy on the conflict screen matches Switch (see above).
- **Sync preview** — **`display_name`** in the list when available; **START: sync & exit** applies then **exits the app**; **B** cancels.
- **Main menu — “needs sync” badge** — Same caching behaviour as Switch (**recompute** after sync actions or save viewer, not every menu draw).
- **Config path** — Single **`GBASYNC_3DS_CONFIG_PATH`** define for **`sdmc:/3ds/gba-sync/config.ini`**.
- **HTTP** — **3 attempts**, **1s** backoff on transient failures.

### Not in v1.1.0 (from internal review)

- **TLS / HTTPS** on 3DS/Switch — not implemented; API key remains on the wire for plain HTTP.
- **Shared scroll-list abstraction** for save viewer / pickers — still duplicated C code; refactor left for a later pass.
- **Global API rate limiting** — not added; upload size cap only.

---

## Upgrade notes

- **Server** — Set **`GBASYNC_MAX_SAVE_UPLOAD_BYTES`** if you need something other than 4 MiB. Deploy the new **`server/`** code (or image).
- **Admin static files** — Deploy **`admin-web/static/`** (or rebuild the image) for upload/history/mobile fixes.
- **Switch / 3DS** — Replace **`gbasync.nro`** / **`gbasync.3dsx`** (or `.cia`) from **`dist/`** or your tagged zip.

Full setup: **`docs/USER_GUIDE.md`**.

---

## Where this is documented elsewhere

| Location | What to read |
|----------|----------------|
| **Repository root `README.md`** | Documentation index (links to this file, **`docs/USER_GUIDE.md`**, **`docs/RELEASE_NOTES_v1.0.0.md`**, etc.). |
| **`docs/USER_GUIDE.md`** | User-facing behavior for **v1.1** console flows (no-baseline conflict copy, needs-sync badge caching, preview keys) and server index notes. Updated when shipped behavior changes. |
| **`server/README.md`** | HTTP API summary; **`GET /saves`** pagination and **`PUT`** size limit are summarized there. |
| **`admin-web/README.md`** | Admin UI routes and upload/history behavior. |

---

## Links

| | |
|--|--|
| **Repository** | [github.com/drippyday/SaveSync](https://github.com/drippyday/SaveSync) |
| **User guide** | `docs/USER_GUIDE.md` |
| **v1.0.0 GitHub release notes** | `docs/GITHUB_RELEASE_NOTES_v1.0.0.md` |
| **v1.0.0 detailed release notes** | `docs/RELEASE_NOTES_v1.0.0.md` |
