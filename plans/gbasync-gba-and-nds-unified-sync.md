# Plan: Unified GBA + NDS save sync (single config, no profile switching)

**Status:** investigation + implementation plan (no code in this step).  
**Goal:** Sync **Game Boy Advance** and **Nintendo DS** `.sav` files in **one** GBAsync run on **3DS** (Twilight Menu) and **Switch**, without maintaining separate `config.ini` copies or manual switching.

**Compatibility:** No requirement to preserve legacy **`save_dir`** / **`rom_dir`** keys — **clean break** acceptable; migrate the single existing install to the new schema by hand.

---

## 0. Recommended config schema (clean break)

Per-platform pairs (omit unused systems):

| Role | Example keys |
|------|----------------|
| Saves | `gba_save_dir`, `nds_save_dir`, `gb_save_dir` |
| ROMs | `gba_rom_dir`, `nds_rom_dir`, `gb_rom_dir` |
| Extension | `gba_rom_extension`, `nds_rom_extension`, `gb_rom_extension` (defaults e.g. `.gba`, `.nds`, `.gb`) |

- **Scan:** union of `*.sav` under each **set** `*_save_dir` (non-recursive by default unless opted in).
- **`game_id` resolution:** knows **which system** from which root the save came from → try matching ROM under the corresponding `*_rom_dir` with that extension → **NDS / GBA / GB** header rules or stem fallback.
- **Drop** monolithic `[sync] save_dir` + `[rom] rom_dir` in favor of this layout (or keep **only** as internal aliases during a short transition — **not** required if breaking compat).

---

## 1. Problem statement

### User constraints

- **Twilight Menu** on 3DS; saves may live under paths such as:
  - `roms/nds/saves/<name>.sav` (NDS)
  - `roms/gb/<name>.sav` (GB / GBA layout as used on-card — exact layout is user-defined)
- ROM trees: `roms/nds`, `roms/gb` (or similar).
- **Requirement:** GBA and NDS **at the same time** — not “sync GBA today, swap config, sync NDS tomorrow.”

### Current product limits

| Area | Today | Blocker for unified GBA+NDS |
|------|--------|-----------------------------|
| **`[sync] save_dir`** | Single directory scanned for `*.sav` | One folder cannot cover `nds/saves` and `gb/` unless a **parent** is chosen and rules are defined for recursion/subfolders. |
| **`[rom] rom_dir` + `rom_extension`** | Single ROM root + one extension (e.g. `.gba`) | NDS ROMs live under **`roms/nds`** with **`.nds`**; GBA under **`roms/gb`** with **`.gba`**. One pair cannot describe both without extension. |
| **ROM header → `game_id` (3DS/Switch)** | **GBA-only** offsets (`0xA0` / `0xAC`) | Feeding **NDS** bytes through GBA parsing yields **wrong or empty** `game_id`. |
| **Bridge `game_id.py`** | Already has **`game_id_from_nds_bytes`** and `.nds` handling in ROM maps | Handheld clients do **not** yet mirror this. |
| **Server** | Opaque blobs + string `game_id` | **No** GBA-only restriction; OK for DS sizes. |
| **Delta / Harmony trim** | **131088 → 131072** for GBA/mGBA | DS uses **per-game** sizes in Harmony — separate concern; no shared trim rule. |

---

## 2. Investigation summary (feasibility)

### 2.1 Server

- **Feasible without schema migration:** `PUT /save/{game_id}` accepts arbitrary binary length; index stores metadata as today.
- **Optional later:** `platform` or `system` hint in metadata for admin UX (not required for MVP).

### 2.2 Bridge / desktop

- **Already:** `game_id_from_nds_bytes`, `game_id_from_rom_bytes` (GBA then NDS fallback in one helper — order matters for ambiguous files), `_rom_sha1_and_game_id` branches on **`.nds`**.
- **Work:** Ensure **`delta_folder_server_sync` / slot maps** can associate DS titles with server `game_id` the same way as GBA (ROM SHA-1, title hints). Validate against real Harmony exports for a few DS games.

### 2.3 Handheld clients (3DS / Switch)

- **Must implement:** Multi-root or multi-pair scanning; **NDS header path** when ROM is `.nds` (mirror `bridge/game_id.py`: title `0x00–0x0B`, code `0x0C–0x0F`).
- **Must define:** How **`game_id`** namespaces GBA vs DS if the same stem exists twice (unlikely if paths differ; possible if user duplicates names — see §3.3).

### 2.4 Twilight Menu

- Affects **paths on SD**, not the wire protocol. Plan should reference **documented TM layouts** (nds-bootstrap `saves` folder next to ROMs vs centralized `saves` under `nds`) and let users set resolved paths in config.
- **No** special-case code for “Twilight” brand string — only path + extension rules.

---

## 3. Design options (choose before coding)

### 3.1 Per-system save + ROM dirs (recommended — see §0)

- **Named keys** `gba_save_dir`, `nds_save_dir`, `gb_save_dir` + matching `*_rom_dir` and optional `*_rom_extension` — **no** comma-separated `save_dirs=` list required for clarity.
- **Scan:** union of all `*.sav` under each **set** save dir (non-recursive vs recursive — **decision:** TM often uses flat `saves/`; default **non-recursive** per root, optional `save_scan_recursive=true` later).
- **Baseline / status / id-map:** **Recommendation:** **one** `.gbasync-baseline` (and status/id-map) **per** `*_save_dir` root, or a **single** merged baseline keyed by full path — pick one in Phase A.

### 3.2 `game_id` resolution with explicit system

- For each discovered `.sav`, **system** is known from which `*_save_dir` contained it (or from longest-prefix match if paths overlap — avoid overlap in config).
- Resolve: `stem` + `nds_rom_dir` / `gba_rom_dir` / `gb_rom_dir` + extension → **NDS / GBA / GB** header parsing or stem slug.
- **Collision:** If the same stem exists in two ROM dirs (user error), **system** from save path disambiguates.

### 3.3 `game_id` collisions

- If **`pokemon-diamond.sav`** and different systems share stems, **filename-only** IDs collide.
- **Mitigation options:**
  - **Prefix by system:** `nds-pokemon-diamond` vs `pokemon-emer-bpee` (only if resolver knows system).
  - **Rely on ROM header** once NDS parsing exists — retail codes differ (`NTR-xxx`).
  - **Document:** “unique save filenames across systems” as soft requirement if using filename-only mode.

### 3.4 Single parent `save_dir` (alternative)

- e.g. `save_dir=sdmc:/roms` with **recursive** scan for `*.sav`.
- **Risk:** picks up unintended `.sav` files; deeper TM folders may include duplicates.
- **Verdict:** possible as **advanced** option; not the default recommendation.

---

## 4. Implementation phases (proposed)

### Phase A — Design + config spec (no device code)

1. Freeze **INI schema** per §0 (`*_save_dir` / `*_rom_dir` / optional `*_rom_extension`); **no** legacy `save_dir`/`rom_dir` requirement.
2. Document **Twilight Menu** example paths in **`docs/USER_GUIDE.md`** (when implemented).
3. List **test matrix:** GBA only, NDS only, mixed, filename-only, ROM header for both.

### Phase B — Handheld: scan + resolve

1. **3DS + Switch:** Parse new config; build **union** of local save files with **full path** or **(root, relative)** for each entry.
2. **`resolve_game_id(save_path, cfg)`:**  
   - Infer **system** from which `*_save_dir` the file lives under.  
   - Look up ROM in the matching `*_rom_dir` with the right extension → **NDS / GBA / GB** header (or stem fallback).
3. **Baseline / id-map:** extend to support multiple roots (per-root files or unified format with path keys — **implementer choice from Phase A**).
4. **Memory / UI:** save viewer lists **all** merged ids; ensure 3DS stack limits still safe (heap patterns as history code).

### Phase C — Bridge + Delta

1. Confirm **`rom_dirs`** in bridge configs include **both** `roms/gb` and `roms/nds` with extensions **`[".gba", ".nds"]`** (or separate maps).
2. **Delta:** validate DS **Harmony** `files[0].size` vs server blob; **no** GBA 131088 trim for DS; add **logging** on size mismatch only.
3. **Slot / title mapping:** extend regression tests for at least one retail DS title in Dropbox export.

### Phase D — Docs + release

1. **`docs/USER_GUIDE.md`**, **`3ds-client/README.md`**, **`switch-client/README.md`**: unified GBA+NDS, TM paths, collision caveats.
2. **Version bump** + release scripts for both consoles.
3. **Optional:** admin UI column “size” / hint for non-GBA (cosmetic).

---

## 5. Risks and non-goals (MVP)

| Risk | Mitigation |
|------|------------|
| Wrong `game_id` if header parsing fails | Strong fallback to stem; optional **manual id map** file later. |
| Large DS saves / slow hash | Already SHA-256 whole file; acceptable; watch 3DS memory for huge lists. |
| Delta conflicts | Same as GBA — document **Cloud** resolution after sync. |
| **3DSi / DSiWare / CIA** | **Out of scope** unless ROM file is a standard `.nds` extract. |

**Non-goals for first ship:** 3DS **VC DSi** special cases, Switch **standalone DS emulators** other than user-pointed paths, **RetroArch** multi-dir unless covered by generic multi-root.

---

## 6. Open questions (resolve in Phase A)

1. **GB / GB Color:** header parsing vs filename-only for `gb_save_dir` (different from GBA).
2. **Baseline file location:** one file per `*_save_dir` vs single merged baseline with path keys.
3. **Recursive scan:** default off or on under each `*_save_dir`.
4. **Switch:** multiple emulator output paths — list all as separate logical `*_save_dir` entries or allow two GBA paths (e.g. `gba_save_dir_2`) if needed later.

---

## 7. References (code anchors)

- NDS header helper (Python): `bridge/game_id.py` — `game_id_from_nds_bytes`, `game_id_from_rom_bytes`, `_rom_sha1_and_game_id`.
- GBA-only handheld parsing: `3ds-client/source/main.c` — `game_id_from_rom_header_bytes`, `resolve_game_id_for_save`; `switch-client/source/main.cpp` — `game_id_from_rom_header`, `resolve_game_id_for_save`.
- GBA trim (Delta only): `bridge/delta_dropbox_sav.py` — `apply_bytes_to_delta` (131088/131072).

---

## 8. Summary

| Workstream | Effort (rough) | Notes |
|------------|----------------|--------|
| Config + multi-dir scan | Medium | Touch both clients + docs |
| NDS header in C/C++ | Low–medium | Mirror Python offsets; test retail ROMs |
| Baseline / id-map multi-root | Medium | No legacy migration if clean break |
| Bridge / Delta | Low–medium | Mostly config + DS spot-checks |
| Server | Low | None required for blob storage |

**Bottom line:** Unified GBA+NDS sync is **feasible**; the **required** product work is **multi-directory save scanning** + **NDS-aware ROM header resolution** (or strict filename-only with documented uniqueness). **Switching config is not required** once those pieces exist.
