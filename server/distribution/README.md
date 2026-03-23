# GBAsync server (Docker release)

This folder matches **`gbasync-server-<version>.zip`** from **`./scripts/release-server.sh <version>`**.

## Contents

| File | Purpose |
|------|---------|
| **`gbasync-server-<version>.tar`** | Docker image (`docker load` then run Compose). |
| **`docker-compose.yml`** | Runs the pre-built image (no `build:` — load the tar first). |
| **`.env.example`** | Template for **`API_KEY`**, Dropbox, admin, paths. |
| **`.env`** | Copy of the example (edit **`API_KEY`** before exposing the server). |

## Quick install

1. **Load the image**

   ```bash
   docker load -i gbasync-server-<version>.tar
   ```

2. **Configure**

   ```bash
   cp -f .env.example .env
   # Edit .env: set API_KEY, paths, optional GBASYNC_ADMIN_PASSWORD, Dropbox, etc.
   ```

3. **Run**

   ```bash
   docker compose up -d
   ```

4. **Check**

   ```bash
   curl http://127.0.0.1:8080/health
   ```

## Paths and data

- Compose sets **`SAVE_ROOT`**, **`INDEX_PATH`**, **`HISTORY_ROOT`** to **`/data/...`** inside the container.
- The sample **`docker-compose.yml`** bind-mounts **`../../save_data`** (from this folder, that is two levels up) to **`/data`**. If you unpacked only this directory, change the volume to something like **`./save_data:/data`** next to this folder.
- Full list of environment variables: **`docs/USER_GUIDE.md`** §2 (repository-root **`.env`**).

## Dropbox / admin

Optional Dropbox bridge and web admin use the **same** **`.env`** as the server. See **`docs/USER_GUIDE.md`** and **`bridge/DROPBOX_SETUP.md`** in the full repository.

## Rebuild from source

Clone the GBAsync repo and use **`server/docker-compose.yml`** with **`docker compose build`** if you need a custom image instead of this tarball.
