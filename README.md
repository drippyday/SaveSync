# GBAsync

**Keep one GBA save in sync across your devices**—your phone, your handhelds, and anything else you use to play. GBAsync is a small **self-hosted** service: you run a server that holds the “official” copy of each `.sav`, and **homebrew apps** on **Nintendo Switch** and **Nintendo 3DS** (plus an optional **desktop bridge** for **Delta** on iOS) push and pull saves when *you* choose—so you can pick up the same game on different hardware without emailing files or copying SD cards by hand.

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

1. You run the **GBAsync server** (usually with Docker). It stores save files and an index.
2. On **Switch** or **3DS**, you install the homebrew **client**, point it at your server in **`config.ini`**, and use the menus to sync.
3. If you use **Delta** on iOS, you can optionally run the **desktop bridge** on a Mac or PC so a folder of `.sav` files stays in step with the server—**or** use **Dropbox-related modes** documented in the user guide so the server talks to Dropbox for you.

Repository layout for contributors: **`server/`** (API + optional **`admin-web/`** UI), **`switch-client/`**, **`3ds-client/`**, **`bridge/`**.

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

**3. Optional — Delta / bridge / Dropbox**

Not everyone needs this. If you sync with **Delta** or **Dropbox**, follow **`docs/USER_GUIDE.md`** (Dropbox modes, bridge setup, and Harmony/Delta quirks are explained there—not in this README).

---

## Documentation

| Doc | Purpose |
|-----|---------|
| **`docs/USER_GUIDE.md`** | Full setup: server, bridge, Switch, 3DS, Dropbox, troubleshooting |
| **`docs/RELEASE_NOTES_v0.1.7.md`** | Latest release (**v0.1.7**): admin dashboard & save management, client save viewer, labels, keep/lock, Delta trim + packaging |
| **`docs/RELEASE_NOTES_v0.1.6.md`** | Prior console-focused release notes |
| **`dist/README.md`** | What’s in release zips and where files go |
| **`admin-web/README.md`** | Enabling and using the optional admin UI (`/admin/ui/`) |
| **`docs/TODO.md`** | What’s shipped vs planned |
| **`docs/HARDWARE_VALIDATION_CHECKLIST.md`** | Optional real-device test matrix (sync, conflicts, history) |
| **`docs/RELEASE.md`** | How maintainers build and package releases |

---

## Building from source

Release scripts (from repo root; **devkitPro** required for Switch/3DS):

```bash
./scripts/release-server.sh v0.1.7
./scripts/release-bridge.sh v0.1.7
./scripts/release-switch.sh v0.1.7
./scripts/release-3ds.sh v0.1.7
```

**Toolchain:** Python 3.11+ (server/bridge), Docker (server), devkitPro (`devkitA64`/`libnx` for Switch, `devkitARM`/`libctru` for 3DS), plus 3DS packaging tools for **`./scripts/release-3ds.sh`** (`makerom`, `bannertool`; `sips` on macOS for banner assets). See **`docs/RELEASE.md`** for detail.

**Smoke test:**

```bash
./scripts/smoke-sync.sh
```

---

## Contributing

See **`docs/TODO.md`** for themes (e.g. HTTPS on consoles, richer admin UI, tests). **`PLAN.md`** and **`IDEA.md`** hold broader planning notes.

---

## License

This project is licensed under the **GBAsync Non-Commercial License 1.0**. See **`LICENSE`**.
