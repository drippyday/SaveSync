# Switch Client (libnx NRO MVP)

Nintendo Switch homebrew sync client using libnx and socket-based HTTP.

## Implemented features

1. Config parsing from `sdmc:/switch/gba-sync/config.ini`
2. Local `.sav` scan from configured save folder(s) — **legacy** single `save_dir`, or **multi-root** GBA / NDS / GB (same layout as the 3DS client)
3. **ROM-derived** `game_id`: GBA header (title + code), **NDS cartridge header** (title + game code at 0x00 / 0x0C), GB/Color **stem-only** when ROM is missing (matches 3DS)
4. HTTP sync operations:
   - `GET /saves`
   - `GET /save/{game_id}` (+ `/meta`)
   - `PUT /save/{game_id}` (`force=1` on uploads from this client)
5. **Auto (A):** If the plan has **no** upload/download/skip-no-baseline/conflict work (only OK and/or locked rows), the app **skips the preview** and prints **Already Up To Date** once (no per-game `OK` lines), then the post-sync menu. Otherwise **preview** lists per-game actions, then **A** runs / **B** cancels; baseline **`.gbasync-baseline`** + SHA-256 (legacy **`.savesync-baseline`** supported); first-run **SKIP** until X/Y seeds baseline; **Conflict** UI (X/Y/B) during apply
6. **Save viewer:** main menu **R** lists local + server ids (server **display_name** when set, else **`game_id`**); **R** toggles lock; **A** history / restore (**R** in history = keep/unkeep); **B** back (sync **preview** is confirm-only — no lock toggle on preview)
7. **Upload (X)** / **download (Y)** pickers with checklist; **+** runs, **B** back
8. **Status line** on main menu (last sync / server / Dropbox) persisted in **`.gbasync-status`** next to the first save root’s baseline
9. Optional **`sync.locked_ids`** — comma-separated `game_id` list skipped on Auto; **R** toggles lock in **Save viewer** (main menu **R**) and **writes `config.ini`** (same path as at launch: `sdmc:/switch/gba-sync/config.ini`)
10. **Post-sync:** **A** main menu, **+** exit app; after **Auto** when the result was **Already Up To Date**, also **Y: reboot now** (uses `spsm`)
11. Atomic write for downloaded saves; resilient HTTP parsing (chunked bodies, `Accept-Encoding: identity`, etc.)
12. Optional **`sync_nds_saves`** (default on) — set `false` to omit the NDS save root and skip DS titles. With **`nds_rom_dir`** set, `.sav` files whose stem matches an `.nds` ROM there are skipped when NDS sync is off (mixed mGBA folder). Without manual config, retail DS saves are often **exactly 512 KiB** (524288 bytes); when NDS sync is off, those files are skipped locally and matching remote rows (using `size_bytes` from `GET /saves`) are ignored. **Optional** **`skip_save_patterns`** substrings still apply when you need to exclude a game that does not match that size.

## Example config

**Legacy (single GBA root):**

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

**Multi-root (GBA + NDS + GB — same idea as 3DS `gba_save_dir` / `nds_save_dir` / `gb_save_dir`):**

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
# Comma-separated list is OK — tried in order (e.g. ``.gb`` then ``.gbc``).
gb_rom_extension=.gb,.gbc
```

**No NDS sync (GBA/GB folder also has DS `.sav` files):** set `sync_nds_saves=false`. The 3DS client does **not** sniff bytes inside `.sav` files — it uses **separate** `nds_save_dir` / `gba_save_dir` so each emulator writes to its own folder. On Switch, if everything lands in one folder, use **`sync_nds_saves=false`**: **512 KiB** saves are treated as DS (typical retail), **nds_rom_dir** skips stems that have a matching `.nds`, and **`skip_save_patterns`** is only needed for edge cases.

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

**VC mode** (Checkpoint-style GBA VC only): `sync.mode=vc` and set `sync.vc_save_dir`; `[rom]` still supplies ROM paths for header IDs.

Baseline files **`.gbasync-baseline`** are written per save root; the app merges them for Auto sync. Downloads are routed to the folder that matches an existing local file, or the root whose ROM path exists for the save stem (same as 3DS).
