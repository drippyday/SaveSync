# `config.ini` reference (3DS)

Path on SD: **`sdmc:/3ds/gba-sync/config.ini`**

Lines starting with **`#`** or **`;`** are comments. Sections: **`[server]`**, **`[sync]`**, **`[rom]`**.

The 3DS build does **not** implement **`sync_nds_saves`** or **`skip_save_patterns`** (those are Switch-only). All other keys below match the parser in **`3ds-client/source/main.c`**.

---

## `[server]`

| Key | Required | Description |
|-----|----------|-------------|
| **`url`** | **Yes** | GBAsync base URL, e.g. **`http://192.168.1.10:8080`**. Must use **`http://`**. No trailing path. |
| **`api_key`** | **Yes** | Same as server **`API_KEY`** (**`X-API-Key`**). |

---

## `[sync]`

| Key | Required | Description |
|-----|----------|-------------|
| **`mode`** | No | **`normal`** (default) — use **`save_dir`** and/or multi-root **`gba_save_dir`** / **`nds_save_dir`** / **`gb_save_dir`**. **`vc`** — use **`vc_save_dir`** only (Checkpoint / VC exports). |
| **`save_dir`** | Usually | Primary **`.sav`** folder when not using separate multi-root dirs. |
| **`vc_save_dir`** | If **`mode=vc`** | VC / Checkpoint export folder. |
| **`gba_save_dir`** | No | GBA **`.sav`** root for multi-root layout. |
| **`nds_save_dir`** | No | NDS **`.sav`** root. |
| **`gb_save_dir`** | No | Game Boy / GBC **`.sav`** root. |
| **`locked_ids`** | No | Comma-separated **`game_id`** values skipped on Auto (case-insensitive). The save viewer can update this line. |

If **`gba_save_dir`**, **`nds_save_dir`**, or **`gb_save_dir`** is set, the app builds **multiple save roots** (each with its own ROM dir/extensions). Otherwise it uses **`save_dir`** + **`[rom]`** as a single root.

---

## `[rom]`

| Key | Required | Description |
|-----|----------|-------------|
| **`rom_dir`** | Usually | ROM search path for **single-root** mode. Trailing slashes removed internally. |
| **`rom_extension`** | No | Default **`.gba`**. Comma-separated values allowed (e.g. **`.gb,.gbc`**). |
| **`gba_rom_dir`** | No | ROM dir for GBA multi-root. |
| **`nds_rom_dir`** | No | ROM dir for NDS. |
| **`gb_rom_dir`** | No | ROM dir for GB/GBC. |
| **`gba_rom_extension`** | No | Default **`.gba`**. |
| **`nds_rom_extension`** | No | Default **`.nds`**. |
| **`gb_rom_extension`** | No | Default **`.gb`**. |

---

## Sidecar files

Next to **`.sav`** files: **`.gbasync-baseline`** (and legacy **`.savesync-baseline`**), **`.gbasync-status`**, **`.gbasync-idmap`** as applicable.

---

See **`docs/USER_GUIDE.md`** in the repo for server setup, VC workflow, and Delta/Dropbox.
