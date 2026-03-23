# GBAsync Switch client (libnx)

Nintendo **Switch** homebrew app that syncs **`.sav`** files with a **GBAsync server** over plain **HTTP** (same API as 3DS). Uses **libnx**; distributed as **`.nro`** for Homebrew Menu.

---

## Prerequisites

- **Hacked Switch** with **Homebrew Menu** (or equivalent) to run **`.nro`**.
- **SD card** (or USB) to copy **`gbasync.nro`** and config.
- A running **GBAsync server** (URL + **API key**). See **`docs/USER_GUIDE.md`**.

---

## Setup (install and configure)

1. **Get a build** from **`dist/switch`** (or build from source — **`docs/RELEASE.md`**). Release zips include **`README.md`** and **`gba-sync/README.md`** (every **`config.ini`** key); the same text lives under **`switch-client/distribution/`** in the repo.
2. **Copy to SD card:**
   - **`gbasync.nro`** → **`sdmc:/switch/gbasync.nro`**
3. **Create config:**

   **`sdmc:/switch/gba-sync/config.ini`**

   Minimal example (single GBA save folder):

   ```ini
   [server]
   url=http://YOUR_SERVER_IP:8080
   api_key=your-api-key

   [sync]
   save_dir=sdmc:/mGBA

   [rom]
   rom_dir=sdmc:/roms/gba
   rom_extension=.gba
   ```

4. **Same network** as the server (or reachable IP). Use **`http://`** — this build does **not** use TLS on the console.
5. Launch **gbasync** from **Homebrew Menu**.

---

## Configuration (reference)

| Section | Purpose |
|---------|---------|
| **`[server]`** | **`url`** — GBAsync base URL. **`api_key`** — must match server **`API_KEY`**. |
| **`[sync]`** | Legacy **`save_dir`** **or** **`gba_save_dir`**, **`nds_save_dir`**, **`gb_save_dir`**. **`locked_ids`**. **`sync_nds_saves`** (default on) — set **`false`** to ignore NDS in a mixed folder. **`skip_save_patterns`** — optional substrings to exclude files. |
| **`[rom]`** | **`gba_rom_dir`**, **`nds_rom_dir`**, **`gb_rom_dir`** and matching **`*_rom_extension`** for header-derived **`game_id`**. |

---

## Features (what the app does)

1. **Config** from **`sdmc:/switch/gba-sync/config.ini`**
2. **Local `.sav` scan** — legacy **`save_dir`** or **multi-root** GBA / NDS / GB (same layout as 3DS)
3. **`game_id`** — **GBA** header, **NDS** cartridge header (**0x00** / **0x0C**), **GB/GBC** header — or **stem** when ROM missing (aligned with 3DS/bridge)
4. **HTTP** — **`GET /saves`**, **`GET`/`PUT /save/{game_id}`**, **`/meta`**, history as needed; **`force=1`** on uploads
5. **Auto (A):** If only OK/locked → **Already Up To Date** (no preview). Else **preview** (non-OK rows) → **A** apply / **B** cancel; **`.gbasync-baseline`** + SHA-256 (legacy **`.savesync-baseline`**); first-run **SKIP**; **conflict** UI (**X/Y/B**)
6. **Save viewer (main menu R):** local ∪ server; **`display_name`**; **R** lock; **A** history; **R** in history = **keep**
7. **Upload (X)** / **download (Y)** pickers with checklist; **+** runs, **B** back
8. **`.gbasync-status`** — last sync / server / Dropbox (next to first save root’s baseline)
9. **`locked_ids`** — skipped on Auto; toggled from **Save viewer**; writes **`config.ini`**
10. **Post-sync** — **A** menu, **+** exit; **Y: reboot** after **Already Up To Date** Auto (uses **`spsm`**)
11. **Atomic** download writes; resilient HTTP (chunked, **`Accept-Encoding: identity`**, etc.)
12. **`sync_nds_saves`** — when **`false`**, **512 KiB** saves treated as DS in mixed folders; **`nds_rom_dir`** skips stems with matching **`.nds`**; see **`skip_save_patterns`** for edge cases

---

## Example configs

### Legacy (single GBA root)

```ini
[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
save_dir=sdmc:/mGBA
# locked_ids=myhack,other-game

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

### Multi-root (GBA + NDS + GB)

```ini
[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
gba_save_dir=sdmc:/mGBA
nds_save_dir=sdmc:/retroarch/saves/Nintendo - Nintendo DS
gb_save_dir=sdmc:/roms/gb/saves

[rom]
gba_rom_dir=sdmc:/roms/gba
nds_rom_dir=sdmc:/roms/nds
gb_rom_dir=sdmc:/roms/gb
gba_rom_extension=.gba
nds_rom_extension=.nds
gb_rom_extension=.gb,.gbc
```

### No NDS sync (mixed folder with DS `.sav` files)

```ini
[sync]
gba_save_dir=sdmc:/mGBA
gb_save_dir=sdmc:/mGBA
sync_nds_saves=false

[rom]
gba_rom_dir=
gb_rom_dir=
nds_rom_dir=sdmc:/roms/nds
gba_rom_extension=.gba
gb_rom_extension=.gb,.gbc
nds_rom_extension=.nds
```

### VC mode (Checkpoint-style GBA VC)

Set **`sync.mode=vc`** and **`sync.vc_save_dir`**; **`[rom]`** still supplies ROM paths for header IDs.

**Baseline files** **`.gbasync-baseline`** are written per save root; merged for Auto. Downloads go to the folder that already has a matching file, or the root whose ROM path matches the stem.

---

## See also

- **`docs/USER_GUIDE.md`** — full install, sync flows, Dropbox, troubleshooting  
- **`dist/README.md`** — release zip layout  
- **`3ds-client/README.md`** — same multi-root ideas on 3DS  
