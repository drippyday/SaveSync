# GBAsync

**Keep one GBA save in sync across your devices**—your phone, your handhelds, and anything else you use to play. GBAsync is a small **self-hosted** service: you run a server that holds the “official” copy of each `.sav`, and **homebrew apps** on **Nintendo Switch** and **Nintendo 3DS** (plus an optional **Delta** / **Dropbox** path on iOS) push and pull saves when *you* choose—so you can pick up the same game on different hardware without emailing files or copying SD cards by hand.

---

## What you get

- **One place for saves** — The server stores each game’s save file and metadata. Your devices sync **to** and **from** that source of truth.
- **Works across emulators and real hardware targets** — Same idea whether you’re on Switch, 3DS, or (with the bridge) Delta on iPhone: **one shared system**, not a separate cloud per app.
- **You stay in control** — **Self-hosted**: your machine, your network, your API keys. No vendor lock-in for *how* you sync GBA saves.
- **Clear sync flows** — **Auto sync** walks through **plan → preview → apply** so you see what would change before it happens. **Upload-only** and **download-only** modes are there when you want a manual, per-game checklist.
- **Per-game locks** — Mark games you don’t want touched in Auto, so a stray sync doesn’t overwrite something you care about.
- **Status at a glance** — A small **status file** next to your saves shows last sync, whether the server was reachable, and Dropbox-related results when you use that integration.
- **Conflicts handled explicitly** — If the copy on the device and the copy on the server both diverged, you get a **conflict** prompt instead of silent corruption.
- **Optional web admin** — With a password set in config, you can open a simple **admin UI** in the browser (saves, conflicts, index tools, **save history** with restore and **keep** pins, optional actions). Handy for troubleshooting without SSH.
- **Version history on the server** — Optional per-game backups when a save is replaced; configurable retention, **pin** favorites so they are not trimmed, and restore from the admin UI or from **Switch/3DS** (then pull the save down to the device).

---

## What it doesn’t try to be (yet)

- Console clients talk to the server over **plain HTTP** in this build—**no TLS** on the device side yet.
- Sync on Switch/3DS is **foreground**: you open the app and run a sync; it’s not a background daemon.

Details and workarounds live in **`docs/USER_GUIDE.md`**.

---

## How it fits together (simple picture)

1. You run the **GBAsync server** (usually with **Docker**). It stores save files and an index. With **Dropbox mode** enabled in `.env`, the **same container** can run the **bridge** logic for Delta/Harmony—no extra **bridge** package required on the server host.
2. On **Switch** or **3DS**, you install the homebrew **client**, point it at your server in **`config.ini`**, and use the menus to sync.
3. If you use **Delta** on iOS, you typically configure **Dropbox** + **`GBASYNC_DROPBOX_MODE=delta_api`** in the **server** `.env` so the container syncs the **Harmony** folder (see **`docs/USER_GUIDE.md`**). Only if you need a **separate** machine running the Python scripts **outside** Docker do you use the standalone **`bridge/`** repo folder or **`dist/bridge`** zip.

Repository layout for contributors: **`server/`** (API + optional **`admin-web/`** UI), **`switch-client/`**, **`3ds-client/`**, **`bridge/`** (also copied into the server Docker image).

---

## Quick start

**1. Start the server**

```bash
cp .env.example .env
cd server
docker compose up -d
```

Saves and `index.json` live under **`save_data/`** at the **repo root** (see `server/docker-compose.yml`). Check health:

```bash
curl http://127.0.0.1:8080/health
```

**2. Install the console builds**

Prebuilt artifacts are described in **`dist/README.md`**. Each console zip includes an **`INSTALL.txt`**. Configure **`gba-sync/config.ini`** with your server URL and API key.

**3. Optional — Delta / Dropbox**

Only if you want the **server** to read/write **Delta’s Dropbox** (Harmony) or a **flat** `.sav` folder in Dropbox: set **`GBASYNC_DROPBOX_MODE`** and credentials in **`.env`** (see **`docs/USER_GUIDE.md`**). **Skip this** for Switch/3DS ↔ server only.

**You do not need** to install or unzip the **`bridge/`** release package when using **Docker** with Dropbox mode—the container already runs those scripts.

---

## Documentation

| Doc | Purpose |
|-----|---------|
| **`docs/USER_GUIDE.md`** | Full setup: server, bridge, Switch, 3DS, Dropbox, troubleshooting |
| **`docs/RELEASE_NOTES_v1.0.0.md`** | **v1.0.0** — full product summary for the first major release |
| **`docs/RELEASE_NOTES_v0.1.*.md`** | Earlier incremental release notes |
| **`dist/README.md`** | What’s in `dist/` release artifacts and how they map to installs |
| **`admin-web/README.md`** | Optional admin UI (`/admin/ui/`) — auth, tabs, related server routes |
| **`server/README.md`** | HTTP API summary and `save_data/` layout |
| **`bridge/README.md`** | `bridge.py`, Harmony helpers, Delta ↔ server sync scripts |
| **`docs/TODO.md`** | What’s shipped vs planned |
| **`docs/HARDWARE_VALIDATION_CHECKLIST.md`** | Optional real-device test matrix (sync, conflicts, history) |
| **`docs/CONSOLE_CLIENT_PERFORMANCE.md`** | Switch/3DS performance notes (HTTP, hashing, safe vs risky ideas) |
| **`docs/RELEASE.md`** | Maintainer packaging (`./scripts/release-*.sh`) and release checklist |

---

## Building from source

Release scripts (from repo root; **devkitPro** required for Switch/3DS):

```bash
./scripts/release-server.sh v1.0.0
./scripts/release-bridge.sh v1.0.0
./scripts/release-switch.sh v1.0.0
./scripts/release-3ds.sh v1.0.0
```

Replace the tag with your release (e.g. `v1.0.0`). **Toolchain:** Python 3.11+ (server/bridge; Docker image uses 3.12), Docker (server), devkitPro (`devkitA64`/`libnx` for Switch, `devkitARM`/`libctru` for 3DS), plus 3DS packaging tools for **`./scripts/release-3ds.sh`** (`makerom`, `bannertool`; `sips` on macOS for banner assets). See **`docs/RELEASE.md`** for detail.

**Smoke test:**

```bash
./scripts/smoke-sync.sh
```

---

## Contributing

See **`docs/TODO.md`** for themes (e.g. HTTPS on consoles, richer admin UI, tests). Background context: **`docs/INITIAL_IDEA.md`**.

---

## Server, Docker, and the `bridge/` folder (important)

**Most users never install the bridge separately.**

- If you run the **GBAsync server with Docker** (`cd server && docker compose up -d`) and set **`GBASYNC_DROPBOX_MODE`** to something other than `off` in your repo-root **`.env`**, the container already includes the **bridge Python scripts** under `/app/bridge`. The entrypoint runs **`write_bridge_config.py`** and a **sidecar** that periodically executes **`delta_dropbox_api_sync.py`** or **`dropbox_bridge.py`**—the same code as in the **`bridge/`** directory in this repo. **You do not need** to unzip `dist/bridge/…` on another machine for that setup.
- **You also do not need** the bridge for **Switch ↔ server** or **3DS ↔ server** only. The consoles talk **HTTP** to the server API; no desktop bridge is involved.

**When you *do* need the standalone `bridge/` tree (or `dist/bridge/gbasync-bridge-*.zip`):**

- You want **`bridge.py`** to watch a **local folder** of plain `.sav` files on a **Mac/PC** (no Docker on that machine), and sync that folder with the server.
- You run **`delta_folder_server_sync.py`** against a **locally synced** Delta Emulator folder (Dropbox desktop app) **outside** Docker.
- You run **`delta_dropbox_api_sync.py`** or **`dropbox_bridge.py`** **manually** on a host that is **not** the Docker server (e.g. a small always-on box without the GBAsync image).

**Optional web admin** (`admin-web/`) is bundled **inside** the Docker image as static files; no separate install.

See **`bridge/README.md`** for script-by-script detail and **`docs/USER_GUIDE.md`** for Dropbox env vars and Harmony behavior.

### Storing saves on disk (including folders synced by *other* apps)

GBAsync is **not** tied to Dropbox for where files live on your machine:

- The **server** writes blobs under **`SAVE_ROOT`** and metadata under **`INDEX_PATH`** / **`HISTORY_ROOT`**. Those paths are **ordinary directories**. You can **bind-mount** (Docker) or point env vars at a folder that **Syncthing**, **Resilio**, **iCloud Drive**, **NFS**, or any other tool keeps in sync—GBAsync just reads/writes files there; the other app handles replication.
- If you want a **second** tree of **plain `.sav` files** (for another workflow) that tracks the server, run **`bridge.py`** on a PC/Mac and set **`delta_save_dir`** to a local folder; point that folder at whatever sync tool you use. See **`bridge/README.md`** (“Local copies…”).

**Dropbox-specific** scripts (`delta_dropbox_api_sync.py`, etc.) are only for **Dropbox’s API** or **Harmony** layout—not required for “save to my disk / my sync folder.”

---

## License

This project is licensed under the **GBAsync Non-Commercial License 1.0**. See **`LICENSE`**.
