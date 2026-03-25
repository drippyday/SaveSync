# GBAsync admin web UI

Static browser UI for operating a running **GBAsync server** (dashboard, saves, conflicts, index routing, optional slot map, actions). **No build step**: plain HTML, CSS, and JavaScript under **`static/`**. The UI is **optional**; the server works fully via HTTP API and console clients without it.

---

## What it does (feature list)

| Area | Purpose |
|------|---------|
| **Dashboard** | Snapshot: Dropbox mode, save/conflict counts, data paths on disk; **max history versions per game** (read/write via settings API). |
| **Saves** | **Table** (desktop) and **cards** (narrow screens): hash preview, size, conflict flag, last modified / server upload time. Per row: **Download** (blob), **Upload** (pick a `.sav` to replace the server copy — same pipeline as `PUT /save` with `force`; success **modal**), **history** modal (**Done** in the modal **header** on narrow screens), **Display name** (prompt). **Drag** rows to reorder when the search filter is **empty**, then **Save row order** → **`PUT /admin/api/save-order`** (same order as **`GET /saves`** for clients). |
| **Conflicts** | Lists saves with `conflict: true`; link to resolve (opens **Actions** tab). |
| **Index routing** | View and **edit** **aliases**, **`rom_sha1`** map, **tombstones**; **Save** writes **`PUT /admin/api/index-routing`**. |
| **Slot map** | If **`GBASYNC_SLOT_MAP_PATH`** (or legacy **`SAVESYNC_SLOT_MAP_PATH`**) points at a JSON file the server can read, shows path and parsed JSON; otherwise explains that it is optional. |
| **Actions** | **Run Dropbox sync once** (`POST /admin/api/dropbox/sync-once`), **resolve conflict** by `game_id`, **delete save** (type `game_id` twice to confirm). |

**Upload behavior:** the UI calls **`PUT /admin/api/save/{game_id}`** with the file bytes. **`filename_hint`** is always **`{game_id}.sav`** (the row you clicked), not the file’s name on disk—so importing e.g. `unbound-0424.sav` into the **`unbound`** row does not register a second `game_id` from the filename. On **HTTPS** or **localhost**, the browser computes **SHA-256**; on **plain HTTP** to a LAN IP, **Web Crypto** may be unavailable — the client **omits** `sha256` in the query and the **server** hashes the body (see **`server/app/admin.py`**). A **fixed** top banner shows **Uploading…** / errors; **Upload complete** uses a **modal** (or **`alert`** if the modal markup is missing).

---

## Setup (enable and open)

1. **Server environment** (repo-root **`.env`**):
   - Set a non-empty **`GBASYNC_ADMIN_PASSWORD`** — this **turns the admin UI on** (unset = admin disabled).
   - Set **`API_KEY`** (used for session cookie HMAC and **`X-API-Key`**). **Or** set **`GBASYNC_ADMIN_SECRET`** for cookie HMAC only; **`API_KEY`** still required for `X-API-Key` unless you also use it for HMAC.
2. **Restart** the server (Docker: `docker compose up -d --build` or restart the container).
3. **Open in a browser** (same host/port as the API):
   - **`http://<host>:8080/admin`** → redirects to **`/admin/ui/`**
   - Or **`http://<host>:8080/admin/ui/`** directly
4. **Sign in** with the admin password.
5. If assets seem cached after an update, **hard-refresh** or bump **`?v=`** on **`app.js`** / **`styles.css`** in **`index.html`** (maintainers do this when shipping UI changes).

**Optional:** **`GBASYNC_SLOT_MAP_PATH`** — host path to Delta slot-map JSON for the **Slot map** tab.

---

## How it is served

The FastAPI app mounts this folder as static files:

- **`/admin/ui/`** → files from **`admin-web/static/`** (e.g. **`index.html`**).
- **`/admin`** → HTTP redirect to **`/admin/ui/`**.
- **`GET /`** in a browser (when **`Accept`** includes **`text/html`**) redirects to **`/admin/ui/`** for convenience.

The **Docker** image copies **`admin-web`** into the container. Local **`uvicorn`** resolves the repo root and mounts the same path.

---

## Authentication

Admin features are **off** until **`GBASYNC_ADMIN_PASSWORD`** is set. If unset, **`GET /admin/api/me`** reports **`admin_enabled: false`** and protected admin APIs return **404**.

When enabled:

1. **Browser login** — You submit the admin password; the server sets an **HttpOnly** cookie **`gbasync_admin_session`**. The value is HMAC-based (**`GBASYNC_ADMIN_SECRET`** if set, else **`API_KEY`**). Nothing sensitive is stored in **`localStorage`**.
2. **Scripts / curl** — The same admin APIs accept **`X-API-Key`** with the same value as the main GBAsync API key.

If **`GBASYNC_ADMIN_PASSWORD`** is set but neither **`API_KEY`** nor **`GBASYNC_ADMIN_SECRET`** is set, **`/admin/api/me`** returns **`misconfigured: true`** (session cannot be formed).

---

## Configuration reference (server)

| Variable | Role |
|----------|------|
| **`GBASYNC_ADMIN_PASSWORD`** | Enables admin UI and **`/admin/api/*`** when non-empty. |
| **`API_KEY`** | Main API key; used for HMAC cookie unless **`GBASYNC_ADMIN_SECRET`** is set; also **`X-API-Key`** for admin APIs. |
| **`GBASYNC_ADMIN_SECRET`** | Optional; overrides **`API_KEY`** for **cookie HMAC only**. |
| **`GBASYNC_SLOT_MAP_PATH`** | Optional path to slot-map JSON for the **Slot map** tab. |

See **`.env.example`** in the repository root.

---

## Developing the UI

Edit files under **`static/`**. Usually a normal browser refresh is enough; bump query strings on **`index.html`** script/link tags when you want to bust caches after a release.

Keep requests **same-origin** to **`/admin/api`** so cookies work without CORS.

---

## Related code

- Server routes: **`server/app/admin.py`** (prefix **`/admin`**).
- Static mount: **`server/app/main.py`** (`admin-web/static`).
