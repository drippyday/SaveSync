# GBAsync Switch — `config.ini` reference

**Path on SD card:** `sdmc:/switch/gba-sync/config.ini` (fixed; the app does not search elsewhere).

Lines starting with `#` or `;` are comments. Keys are **case-sensitive** as shown.

---

## `[server]`

| Key | Required | Description |
|-----|----------|-------------|
| **`url`** | **Yes** | GBAsync base URL, e.g. `http://192.168.1.10:8080`. Must use **`http://`** (no TLS in this build). No trailing path. |
| **`api_key`** | **Yes** | Same value as the server’s **`API_KEY`** (sent as **`X-API-Key`**). |

---

## `[sync]`

| Key | Required | Description |
|-----|----------|-------------|
| **`mode`** | No | **`normal`** (default) — use **`save_dir`** or multi-root **`gba_save_dir`** / **`nds_save_dir`** / **`gb_save_dir`**. **`vc`** — use **`vc_save_dir`** only (Checkpoint-style exports); multi-root keys are ignored. |
| **`save_dir`** | One of the save roots | Legacy single folder for **`.sav`** files (typically GBA). Ignored if **`gba_save_dir`**, **`nds_save_dir`**, or **`gb_save_dir`** is set (multi-root mode). |
| **`vc_save_dir`** | If **`mode=vc`** | Folder scanned when **`mode=vc`** (instead of **`save_dir`**). |
| **`gba_save_dir`** | Multi-root | GBA save folder when using separate roots. |
| **`nds_save_dir`** | Multi-root | NDS save folder when using separate roots. |
| **`gb_save_dir`** | Multi-root | GB/GBC save folder when using separate roots. |
| **`sync_nds_saves`** | No | **`true`** / **`1`** / **`yes`** (default) — include NDS **`*.sav`** in **`nds_save_dir`** (or mixed folders). **`false`** / **`0`** / **`no`** / **`off`** — skip NDS mapping (e.g. mixed folder with 512 KiB DS saves you do not want synced). |
| **`skip_save_patterns`** | No | Comma-separated **substrings** (case-insensitive). If a save **filename** contains any token, that file is **skipped** for sync. |
| **`locked_ids`** | No | Comma-separated **`game_id`** values **skipped on Auto sync** (still available in manual upload/download). The Save viewer can rewrite this line when you toggle locks. |

---

## `[rom]`

Used to map **`.sav`** stems to ROM files so **`game_id`** comes from **ROM headers** (GBA / NDS / GB) instead of only the filename.

| Key | Required | Description |
|-----|----------|-------------|
| **`rom_dir`** | Legacy | With single **`save_dir`**, ROM search path for **`rom_extension`**. |
| **`rom_extension`** | Legacy | With single **`save_dir`**, e.g. **`.gba`** (comma-separated list allowed in code). |
| **`gba_rom_dir`** | Multi-root | GBA ROM directory (pairs with **`gba_save_dir`**). |
| **`nds_rom_dir`** | Multi-root | NDS ROM directory. |
| **`gb_rom_dir`** | Multi-root | GB/GBC ROM directory. |
| **`gba_rom_extension`** | No | Default **`.gba`** if unset. |
| **`nds_rom_extension`** | No | Default **`.nds`** if unset. |
| **`gb_rom_extension`** | No | Default **`.gb`**; can be **`.gb,.gbc`**. |

If ROMs are missing, the client falls back to a **normalized save filename stem** for **`game_id`**.

---

## See also

- Repo **`switch-client/README.md`** — examples (multi-root, `sync_nds_saves=false`, etc.)
- **`docs/USER_GUIDE.md`** — server setup and troubleshooting
