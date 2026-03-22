# GBAsync v0.1.8 — NDS + Game Boy / Game Boy Color sync (clients & bridge)

Release **v0.1.8** adds **unified multi-system save sync** for **NDS** (`.nds` cartridge headers) and **Game Boy / GBC** (`.gb` / `.gbc` DMG headers) alongside **GBA**, with matching **bridge** ROM identity so **Delta Harmony** / Dropbox can map the same `game_id` keys as consoles.

---

## Bridge (`bridge/game_id.py`)

- **NDS:** unchanged model — `game_id_from_nds_bytes` @ ROM offset `0x00` / `0x0C`; `_rom_sha1_and_game_id` selects the NDS parser when the file extension is **`.nds`**.
- **GB / GBC:** new **`game_id_from_gb_bytes`** using the cartridge title region @ **`0x0134`** (with CGB flag handling @ **`0x0143`**). **`.gb` / `.gbc`** files no longer fall through to the GBA header parser (which could not derive a stable id).
- **`build_rom_sha1_to_game_id`**, **`GameIdResolver`**, and **`game_id_from_rom_bytes`** (used when indexing **Delta `Game-*-game`** ROM blobs) now align **ROM SHA-1 → `game_id`** for GB/GBC titles such as classic **Pokémon Red/Blue** so Harmony rows can match server keys like **`pokemon-red`** when **`GBASYNC_ROM_EXTENSIONS`** includes **`.gb`** / **`.gbc`** and ROM dirs are configured.

---

## Switch (`switch-client`)

- **Multi-root config:** optional **`gba_*`**, **`nds_*`**, and **`gb_*`** save/ROM directories and per-system ROM extensions (including comma-separated **`.gb,.gbc`**).
- **NDS:** derives **`game_id`** from the **NDS cartridge header** (mirrors Python). **`sync_nds_saves`** can disable NDS handling; optional **skip** heuristics help **mixed mGBA folders** (e.g. retail **512 KiB** NDS save size, `skip_save_patterns`).
- **Game Boy:** resolves ids from **GB ROM headers** when the save root is **GB**; shared **`.gbasync-idmap`** / dedupe behavior across roots stays consistent with earlier multi-root work.

---

## 3DS (`3ds-client`)

- **`mode=normal`:** optional **`gba_*`**, **`nds_*`**, **`gb_*`** save and ROM roots in one session (same header logic as the bridge). **`mode=vc`:** unchanged **VC** layout.

---

## Server

- **FastAPI** app metadata version set to **0.1.8**.
- **Dropbox bridge subprocess** no longer captures stdout/stderr, so **`[dropbox] pull` / `[server_delta]`** lines from **`delta_dropbox_api_sync.py`** appear in the same log stream as the server when sync runs from uploads or the admin **Sync once** action.

---

## Docs

- **`docs/DROPBOX_SYNC_PERFORMANCE_PLAN.md`** — future work to reduce full-folder Dropbox API pull cost (no behavior change in this tag).

---

## Artifacts

```bash
./scripts/release-server.sh v0.1.8
./scripts/release-bridge.sh v0.1.8
./scripts/release-switch.sh v0.1.8
./scripts/release-3ds.sh v0.1.8
```

- **`dist/server/gbasync-server-v0.1.8.tar`** — Docker image (includes **`admin-web/`** static UI).
- **`dist/bridge/gbasync-bridge-v0.1.8.zip`**
- **`dist/switch/gbasync-switch-v0.1.8/`** — `gbasync.nro`, `.nacp` metadata **0.1.8**, `INSTALL.txt`, sample `gba-sync/config.ini`
- **`dist/3ds/gbasync-3ds-v0.1.8/`** — `gbasync.3dsx`, `gbasync.cia`, `INSTALL.txt`, sample `gba-sync/config.ini`

Switch **`.nacp`** version comes from **`switch-client/Makefile`** `APP_VERSION` (**0.1.8**).

---

## Upgrade

- **Docker:** rebuild/load **`gbasync-server-v0.1.8.tar`**; ensure **`GBASYNC_ROM_EXTENSIONS`** (and bridge JSON **`rom_extensions`**) lists **`.gb`** / **`.gbc`** / **`.nds`** as needed for your library.
- **Consoles:** deploy **`gbasync.nro`** / **`gbasync.3dsx`** (or `.cia`); extend **`config.ini`** with **`[sync]`** / **`[rom]`** keys for NDS and GB paths if you use them.
- **v0.1.7** users: safe upgrade; configure new roots and ROM extensions for GB/NDS before expecting Delta/server mapping for those systems.

---

## See also

- **v0.1.7** (admin, save viewer, labels, Delta mGBA trim): **`docs/RELEASE_NOTES_v0.1.7.md`**
- Roadmap context: **`plans/gbasync-gba-and-nds-unified-sync.md`**
