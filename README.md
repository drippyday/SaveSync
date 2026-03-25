# GBAsync

**Keep one GBA save in sync across your devices.** GBAsync is a small self-hosted service: you run a server that holds the authoritative copy of each `.sav`, and homebrew apps on **Nintendo Switch** and **Nintendo 3DS** push and pull saves when you choose. An optional Dropbox/Delta path covers iOS via Delta emulator.

---

## What you get

- **One source of truth** — The server stores each game's save and metadata. Your devices sync to and from it.
- **Works across emulators and real hardware** — Switch, 3DS, or Delta on iPhone all share the same system, not a separate cloud per app.
- **Self-hosted** — Your machine, your network, your API keys. No vendor lock-in.
- **Deliberate sync flows** — Auto sync walks through **plan → preview → apply** so you see what would change before it happens. Upload-only and download-only modes are available for manual, per-game control.
- **Per-game locks** — Mark games you don't want touched in Auto sync.
- **Conflict handling** — If both the device and server copies diverged, you get a conflict prompt instead of silent corruption.
- **Version history** — Optional per-game backups when a save is replaced; configurable retention, pin favorites to protect them from trimming, and restore from the admin UI or from Switch/3DS.
- **Optional web admin** — A simple browser UI for browsing saves, resolving conflicts, managing history, and running index tools. No SSH required.

---

## Current limitations

- Console clients communicate over plain HTTP — no TLS on the device side yet.
- Sync on Switch/3DS is foreground only; open the app and run it manually.

Details and workarounds are in **`docs/USER_GUIDE.md`**.

---

## How it fits together

1. Run the **GBAsync server** (usually with Docker). It stores save files and an index. With `GBASYNC_DROPBOX_MODE` set in `.env`, the same container handles the Delta/Harmony bridge — no separate installation needed.
2. On **Switch** or **3DS**, install the homebrew client, point it at your server in `config.ini`, and use the menus to sync.
3. If you use **Delta on iOS**, configure Dropbox + `GBASYNC_DROPBOX_MODE=delta_api` in the server `.env`. See **`docs/USER_GUIDE.md`** for the full Harmony setup.

Repository layout: **`server/`** (API + optional `admin-web/` UI), **`switch-client/`**, **`3ds-client/`**, **`bridge/`** (also copied into the Docker image).

---

## Quick start

**1. Start the server**

```bash
cp .env.example .env
cd server
docker compose up -d
```

Saves and `index.json` live under `save_data/` at the repo root. Check health:

```bash
curl http://127.0.0.1:8080/health
```

**2. Install the console builds**

Prebuilt artifacts are described in **`dist/README.md`**. Each console zip includes a full `config.ini` reference — edit it with your server URL and API key.

**3. Optional — Delta / Dropbox**

To have the server read/write Delta's Dropbox (Harmony) folder, set `GBASYNC_DROPBOX_MODE` and credentials in `.env`. See **`docs/USER_GUIDE.md`**. Skip this for Switch/3DS ↔ server only.

---

## Do I need the `bridge/` package?

**Most users don't.** The Docker image already includes the bridge scripts, and Switch/3DS consoles talk directly to the server API with no bridge involved.

You need the standalone `bridge/` tree (or `dist/bridge/gbasync-bridge-*.zip`) only if you want to run bridge scripts **outside Docker** — for example, watching a local `.sav` folder on a Mac/PC, or running `delta_dropbox_api_sync.py` on a separate always-on machine that doesn't run the GBAsync Docker image.

See **`bridge/README.md`** for script details and **`docs/USER_GUIDE.md`** for Dropbox env vars.

---

## Documentation

| Doc | Purpose |
|-----|---------|
| **`docs/USER_GUIDE.md`** | Full setup: server, bridge, Switch, 3DS, Dropbox, troubleshooting |
| **`docs/GITHUB_RELEASE_NOTES_v1.1.0.md`** | v1.1.0 shipped changes (admin, server, Switch, 3DS) |
| **`docs/RELEASE_NOTES_v1.0.0.md`** | v1.0.0 full product summary |
| **`docs/GITHUB_RELEASE_NOTES_v1.0.0.md`** | v1.0.0 GitHub-style release notes |
| **`docs/RELEASE_NOTES_v0.1.*.md`** | Earlier incremental release notes |
| **`dist/README.md`** | Release artifacts and how they map to installs |
| **`admin-web/README.md`** | Optional admin UI — auth, tabs, related server routes |
| **`server/README.md`** | HTTP API summary and `save_data/` layout |
| **`bridge/README.md`** | `bridge.py`, Harmony helpers, Delta ↔ server sync scripts |
| **`docs/TODO.md`** | What's shipped vs planned |
| **`docs/IDEAS_AND_FUTURE_FEATURES.md`** | User-facing feature ideas & future polish (not a roadmap) |
| **`docs/HARDWARE_VALIDATION_CHECKLIST.md`** | Real-device test matrix |
| **`docs/CONSOLE_CLIENT_PERFORMANCE.md`** | Switch/3DS performance notes |
| **`docs/RELEASE.md`** | Maintainer packaging and release checklist |

---

## Building from source

Requires devkitPro for Switch/3DS builds:

```bash
./scripts/release-server.sh v1.0.0
./scripts/release-bridge.sh v1.0.0
./scripts/release-switch.sh v1.0.0
./scripts/release-3ds.sh v1.0.0
```

**Toolchain:** Python 3.11+ (server/bridge; Docker image uses 3.12), Docker, devkitPro (`devkitA64`/`libnx` for Switch, `devkitARM`/`libctru` for 3DS), plus `makerom` and `bannertool` for 3DS packaging (`sips` on macOS for banner assets). See **`docs/RELEASE.md`** for details.

**Smoke test:**

```bash
./scripts/smoke-sync.sh
```

---

## Contributing

See **`docs/TODO.md`** for open themes (HTTPS on consoles, richer admin UI, tests). Casual user-perspective ideas: **`docs/IDEAS_AND_FUTURE_FEATURES.md`**. Background: **`docs/INITIAL_IDEA.md`**.

---

## License

Licensed under the **GBAsync Non-Commercial License 1.0**. See `LICENSE`.