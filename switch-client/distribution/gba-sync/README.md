# `config.ini` reference (Switch)

Path on SD: **`sdmc:/switch/gba-sync/config.ini`**

Lines starting with **`#`** or **`;`** are comments. Sections: **`[server]`**, **`[sync]`**, **`[rom]`**.

---

## `[server]`

| Key | Required | Description |
|-----|----------|-------------|
| **`url`** | **Yes** | GBAsync base URL, e.g. **`http://192.168.1.10:8080`**. Must use **`http://`** (no TLS in this build). No trailing path. |
| **`api_key`** | **Yes** | Same value as server **`API_KEY`**. Sent as **`X-API-Key`**. |

---

## `[sync]`

| Key | Required | Description |
|-----|----------|-------------|
| **`mode`** | No | **`normal`** (default) — use **`save_dir`** and/or multi-root dirs below. **`vc`** — use **`vc_save_dir`** only (Checkpoint-style VC exports). |
| **`save_dir`** | Usually | Primary folder for **`.sav`** files when not using separate **`gba_save_dir`** / **`nds_save_dir`** / **`gb_save_dir`**. Default-style single root (often mGBA). |
| **`vc_save_dir`** | If **`mode=vc`** | Where VC/exported **`.sav`** files live. |
| **`gba_save_dir`** | No | If set (with other multi-root dirs), enables **multi-root** sync: this root is for GBA saves. |
| **`nds_save_dir`** | No | NDS **`.sav`** root (paired with **`nds_rom_dir`** / **`nds_rom_extension`**). |
| **`gb_save_dir`** | No | Game Boy / GBC **`.sav`** root (paired with **`gb_rom_dir`** / **`gb_rom_extension`**). |
| **`sync_nds_saves`** | No | **`true`** / **`1`** / **`yes`** (default) — include the NDS root. **`false`** / **`0`** / **`no`** / **`off`** — omit NDS root; **`.sav`** stems that match an NDS ROM in **`nds_rom_dir`** can be skipped. |
| **`skip_save_patterns`** | No | Comma-separated **lowercased substrings**. If a save **stem** contains any, that file is skipped (all roots). Example: **`backup,temp`**. |
| **`locked_ids`** | No | Comma-separated **`game_id`** values (case-insensitive) excluded from **Auto** upload/download. The app can rewrite this line from the save viewer (**R**). |

**Multi-root:** If any of **`gba_save_dir`**, **`nds_save_dir`**, or **`gb_save_dir`** is non-empty, those roots are used **instead of** the legacy single **`save_dir`** + **`[rom]`** pair (see code: **`build_save_roots`**).

---

## `[rom]`

Used for **`game_id`** from ROM headers (GBA / NDS / GB) and for picking download targets. **`rom_extension`** fields may be **comma-separated** (e.g. **`.gb,.gbc`**).

| Key | Required | Description |
|-----|----------|-------------|
| **`rom_dir`** | Usually | Folder to search for ROMs when using the **single-root** layout (**`save_dir`** only). |
| **`rom_extension`** | No | Default **`.gba`** for single-root. Comma-separated list allowed. |
| **`gba_rom_dir`** | No | ROM directory for the **GBA** multi-root entry. |
| **`nds_rom_dir`** | No | ROM directory for **NDS** titles. |
| **`gb_rom_dir`** | No | ROM directory for **GB / GBC** titles. |
| **`gba_rom_extension`** | No | Default **`.gba`**. |
| **`nds_rom_extension`** | No | Default **`.nds`**. |
| **`gb_rom_extension`** | No | Default **`.gb`**. |

If a ROM is missing, the app may fall back to a **normalized save filename** for **`game_id`** (less reliable for cross-device matching).

---

## Sidecar files

Next to **`.sav`** files, the app may read/write:

- **`.gbasync-baseline`** — SHA-256 baseline for Auto sync (legacy **`.savesync-baseline`** still read).
- **`.gbasync-status`** — Short last-sync status text for the UI.
- **`.gbasync-idmap`** — Maps save stems to **`game_id`** when needed.

---

See the repository **`docs/USER_GUIDE.md`** for server setup and Delta/Dropbox.
