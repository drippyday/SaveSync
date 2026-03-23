# GBAsync 3DS — `config.ini` reference

**Path on SD card:** `sdmc:/3ds/gba-sync/config.ini` (fixed; the app does not search elsewhere).

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
| **`mode`** | No | **`normal`** (default) — use **`save_dir`** or multi-root **`gba_save_dir`** / **`nds_save_dir`** / **`gb_save_dir`**. **`vc`** — use **`vc_save_dir`** only (Virtual Console / Checkpoint export workflow); multi-root keys are **not** used. |
| **`save_dir`** | One of the save roots | Folder for **`.sav`** files in **`mode=normal`** when **no** multi-root dirs are set. |
| **`vc_save_dir`** | If **`mode=vc`** | Folder used instead of **`save_dir`** for VC-style workflows. |
| **`gba_save_dir`** | Multi-root | If **any** of **`gba_save_dir`**, **`nds_save_dir`**, **`gb_save_dir`** is set, the client uses **only** those roots; legacy **`save_dir`** is ignored. |
| **`nds_save_dir`** | Multi-root | NDS saves directory. |
| **`gb_save_dir`** | Multi-root | GB/GBC saves directory. |
| **`locked_ids`** | No | Comma-separated **`game_id`** values **skipped on Auto sync**. The Save viewer can update this list when you toggle locks. |

**Note:** The 3DS client does **not** read **`sync_nds_saves`** or **`skip_save_patterns`** (Switch-only options).

---

## `[rom]`

| Key | Required | Description |
|-----|----------|-------------|
| **`rom_dir`** | Legacy | With single **`save_dir`**, ROM search path for **`rom_extension`**. |
| **`rom_extension`** | Legacy | e.g. **`.gba`**. |
| **`gba_rom_dir`** | Multi-root | GBA ROM directory. |
| **`nds_rom_dir`** | Multi-root | NDS ROM directory. |
| **`gb_rom_dir`** | Multi-root | GB/GBC ROM directory. |
| **`gba_rom_extension`** | No | Default **`.gba`**. |
| **`nds_rom_extension`** | No | Default **`.nds`**. |
| **`gb_rom_extension`** | No | Default **`.gb`**; can list **`.gb,.gbc`**. |

If ROMs are missing, the client falls back to a **normalized save filename stem** for **`game_id`**.

---

## See also

- Repo **`3ds-client/README.md`** — examples and VC workflow
- **`docs/USER_GUIDE.md`** — full guide and troubleshooting
