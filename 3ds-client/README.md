# GBAsync 3DS client (libctru)

Nintendo **3DS** homebrew app that syncs **`.sav`** files with a **GBAsync server** over plain **HTTP** (same API as Switch). Uses **libctru** networking; distributed as **`.3dsx`** (and optionally **`.cia`** from release scripts).

---

## Prerequisites

- **Hacked 3DS** with **Homebrew Launcher** (or equivalent) able to run **`.3dsx`**.
- **SD card** accessible from PC to copy files.
- A running **GBAsync server** (URL + **API key**). See **`docs/USER_GUIDE.md`**.

---

## Setup (install and configure)

1. **Get a build** from **`dist/3ds`** (or build from source — **`docs/RELEASE.md`**). Release zips include **`README.md`** and **`gba-sync/README.md`** (full **`config.ini`** reference); see **`3ds-client/distribution/`** in the repo.
2. **Copy to SD card:**
   - **`gbasync.3dsx`** → **`sdmc:/3ds/gbasync.3dsx`**
   - Optional: install **`gbasync.cia`** with FBI if your release includes it.
3. **Create config** (sample is often in the release zip):

   **`sdmc:/3ds/gba-sync/config.ini`**

   Minimal example:

   ```ini
   [server]
   url=http://YOUR_SERVER_IP:8080
   api_key=your-api-key

   [sync]
   mode=normal
   save_dir=sdmc:/mGBA

   [rom]
   rom_dir=sdmc:/roms/gba
   rom_extension=.gba
   ```

4. **Same Wi-Fi** as the server (or reachable IP). No TLS on the device in this build — use **`http://`** in **`url`**.
5. Launch **gbasync** from **Homebrew Launcher**.

---

## Configuration (reference)

| Section | Purpose |
|---------|---------|
| **`[server]`** | **`url`** — base URL of GBAsync (no trailing path). **`api_key`** — must match server **`API_KEY`**. |
| **`[sync]`** | **`mode=normal`** vs **`mode=vc`**. **`save_dir`** or multi-root **`gba_save_dir`**, **`nds_save_dir`**, **`gb_save_dir`**. **`vc_save_dir`** for VC mode. **`locked_ids`** — comma-separated **`game_id`** skipped on Auto. |
| **`[rom]`** | ROM search paths for **header-derived `game_id`** (GBA, NDS, GB/GBC). See examples below. |

---

## Features (what the app does)

1. **Config** from **`sdmc:/3ds/gba-sync/config.ini`**
2. **Local `.sav` scan** — **`mode=normal`** or **`mode=vc`**; multi-root **GBA / NDS / GB** when **`gba_*` / `nds_*` / `gb_*`** dirs are set
3. **`game_id`** — **ROM header** when **`[rom]`** is configured (GBA **title + code**, **NDS** cart header, **GB/GBC** header); else **normalized filename stem**
4. **HTTP** — **`GET /saves`**, **`GET`/`PUT /save/{game_id}`**, **`/meta`**, history endpoints as needed; **`force=1`** on uploads from this client
5. **Auto (A):** If nothing to do except OK/locked → **Already Up To Date** (skips preview). Else **plan → preview → apply**; **`.gbasync-baseline`** + SHA-256 (legacy **`.savesync-baseline`**); first-run **SKIP** until baseline is seeded; **conflict** UI (**X/Y/B**)
6. **Save viewer (main menu R):** local ∪ server list; **`display_name`** when set; **R** toggle **lock**; **A** history/restore; **R** in history = **keep** unkeep
7. **Upload / download** pickers — **START**/**X** (upload) or **START**/**Y** (download) batch; **B** back
8. **Status** — main menu line; **`.gbasync-status`** next to baseline in the active save folder
9. **`locked_ids`** — skipped on Auto; edited from **Save viewer**
10. **Post-sync** — **A** menu, **START** exit; **Y: reboot** when applicable
11. **Atomic writes**; bottom-screen UI; resilient HTTP (chunked / encoding / JSON tolerance)

---

## Example configs

### Single GBA root (legacy)

```ini
[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/mGBA
vc_save_dir=sdmc:/3ds/Checkpoint/saves
# locked_ids=myhack,other-game

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

- **`mode=normal`**: scan **`save_dir`** **or** multi-root keys below  
- **`mode=vc`**: scan **`vc_save_dir`** only; **`[rom]`** still identifies ROMs. Multi-root keys are **not** used in VC mode.

### GBA + NDS + GB (`mode=normal` only)

If **any** of **`gba_save_dir`**, **`nds_save_dir`**, or **`gb_save_dir`** is set, the client scans **only** those directories. Legacy **`save_dir`** is ignored in that case.

```ini
[sync]
mode=normal
gba_save_dir=sdmc:/roms/gba/saves
nds_save_dir=sdmc:/roms/nds/saves

[rom]
gba_rom_dir=sdmc:/roms/gba
gba_rom_extension=.gba
nds_rom_dir=sdmc:/roms/nds
nds_rom_extension=.nds
gb_rom_extension=.gb,.gbc
```

---

## See also

- **`docs/USER_GUIDE.md`** — full install, sync flows, Dropbox, troubleshooting  
- **`dist/README.md`** — release zip layout  
- **`switch-client/README.md`** — same multi-root ideas on Switch  
