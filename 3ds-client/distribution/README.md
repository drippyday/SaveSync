# GBAsync — Nintendo 3DS homebrew

## Install

1. Copy **`gbasync.3dsx`** to **`sdmc:/3ds/gbasync.3dsx`** (Homebrew Launcher).
2. Optional: install **`gbasync.cia`** with FBI for a HOME Menu shortcut.
3. Copy the entire **`gba-sync`** folder to **`sdmc:/3ds/gba-sync/`** so the config path is:

   **`sdmc:/3ds/gba-sync/config.ini`**

4. Edit **`gba-sync/config.ini`** (see **`gba-sync/README.md`** for every option).
5. Launch **gbasync** from Homebrew Launcher (or the installed title).

## Bundled files

| File | Purpose |
|------|---------|
| **`gbasync.3dsx`** | Application (Homebrew Launcher). |
| **`gbasync.cia`** | Optional CIA for HOME Menu. |
| **`gbasync.smdh`** | Icon/metadata for the launcher. |
| **`gba-sync/config.ini`** | **Required** — server URL, API key, save/ROM paths. |
| **`gba-sync/README.md`** | Full **`config.ini`** reference. |

## Controls (summary)

| Input | Action |
|-------|--------|
| **A** | Auto sync (preview → apply). |
| **X** | Upload-only. |
| **Y** | Download-only. |
| **SELECT** | Ask server to run Dropbox sync-once (if enabled server-side). |
| **START** | Exit. |

Use **`http://`** URLs only in this build (no TLS on device).

## Troubleshooting

- **`401` / unauthorized:** **`api_key`** must match the server **`API_KEY`**.
- **No saves:** Check **`save_dir`** / **`mode`** and that **`.sav`** files exist.
- **VC workflow:** Use **`mode=vc`** and **`vc_save_dir`**; export/import with Checkpoint (or similar) as described in **`docs/USER_GUIDE.md`**.
