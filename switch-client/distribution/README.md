# GBAsync — Nintendo Switch homebrew

## Install

1. Copy **`gbasync.nro`** to **`sdmc:/switch/gbasync.nro`** on your SD card.
2. Copy the entire **`gba-sync`** folder to **`sdmc:/switch/gba-sync/`** so the config path is:

   **`sdmc:/switch/gba-sync/config.ini`**

3. Edit **`gba-sync/config.ini`** (see **`gba-sync/README.md`** for every option).
4. Launch **gbasync** from the Homebrew Menu.

## Bundled files

| File | Purpose |
|------|---------|
| **`gbasync.nro`** | Application (run from Homebrew Menu). |
| **`gbasync.nacp`** | Title/author/version metadata. |
| **`gbasync.elf`** | Debug / tooling (not needed on SD for normal play). |
| **`gba-sync/config.ini`** | **Required** — server URL, API key, save/ROM paths. |
| **`gba-sync/README.md`** | Full **`config.ini`** reference. |

## Controls (summary)

| Input | Action |
|-------|--------|
| **A** | Auto sync (preview → apply). |
| **X** | Upload-only (pick games). |
| **Y** | Download-only (pick games). |
| **−** | Ask server to run Dropbox sync-once (if enabled server-side). |
| **+** | Exit (from main menu). |

Use **`http://`** URLs only in this build (no TLS on device).

## Troubleshooting

- **`401` / unauthorized:** **`api_key`** in **`config.ini`** must match the server **`API_KEY`**.
- **No saves:** Check **`save_dir`** / multi-root paths and that **`.sav`** files exist there.
- **Wrong `game_id`:** Set **`[rom]`** paths so ROM headers can be read; see **`gba-sync/README.md`**.
