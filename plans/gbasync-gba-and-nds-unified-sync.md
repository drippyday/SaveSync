# Plan: Unified GBA + NDS (+ GB) save sync (single config, no profile switching)

**Status:** **3DS `mode=normal`:** **`gba_*` / `nds_*` / `gb_*`** save + ROM roots, NDS cartridge header IDs, per-root baselines. **`mode=vc`:** unchanged (`vc_save_dir` + `[rom]`). **Switch:** **GBA only** — single **`save_dir`** + **`rom_dir`** (no NDS, no multi-root, no DS sync).

**Goal:** Sync **GBA** and **NDS** `.sav` in **one** session on **3DS** without swapping config. **Switch** stays a **GBA-only** single-folder client.

**Compatibility:** **Clean break** for `config.ini` is acceptable — no obligation to keep monolithic `[sync] save_dir` / `[rom] rom_dir` forever. Migrate existing installs by hand once the new schema ships.

---

## 0. Snapshot — current codebase (verify before coding)

Use this section to avoid stale assumptions.

| Layer | GBA | NDS | Notes |
|-------|-----|-----|--------|
| **Server** | Blob + string `game_id` | Same | No format restriction; large DS carts OK. |
| **Bridge** `bridge/game_id.py` | `game_id_from_gba_bytes` @ 0xA0 / 0xAC | `game_id_from_nds_bytes` @ 0x00 / 0x0C | `_rom_sha1_and_game_id` picks parser by **`.nds`** suffix. `game_id_from_rom_bytes` tries GBA then NDS. |
| **Switch** `switch-client/source/main.cpp` | GBA header @ 0xA0 / 0xAC | **Not supported** | Single **`save_dir`** / **`rom_dir`** / **`rom_extension`** only. |
| **3DS** `3ds-client/source/main.c` | `game_id_from_gba_rom_header_bytes` | `game_id_from_nds_rom_header_bytes` | **`mode=vc`:** single GBA root (`vc_save_dir` + `[rom]`). **`mode=normal`:** optional multi-root **`gba_*` / `nds_*` / `gb_*`**; **`scan_all_local_saves`**. |
| **Delta trim** `bridge/delta_dropbox_sav.py` | 131088 → 131072 when Harmony expects 128 KiB | **Do not** apply GBA trim to DS blobs | DS sizes are **per-title** in Harmony; log mismatches, don’t reuse GBA rule. |

**Implication:** Multi-system sync is implemented on **3DS** only. **Switch** remains GBA-only; use **3DS** (or server/bridge) for NDS + GBA combined workflows.

---

## 1. Recommended config schema (clean break)

Per-system **optional** pairs (omit systems you don’t use):

| Role | Keys |
|------|------|
| Saves | `gba_save_dir`, `nds_save_dir`, `gb_save_dir` |
| ROMs | `gba_rom_dir`, `nds_rom_dir`, `gb_rom_dir` |
| Extensions | `gba_rom_extension`, `nds_rom_extension`, `gb_rom_extension` (defaults e.g. `.gba`, `.nds`, `.gb`) |

**Scan:** union of `*.sav` under each **set** `*_save_dir` (see §3.1 for recursion default).

**`game_id` resolution:** the **system** is determined by **which configured save root** contained the file (longest-prefix wins if paths are nested — **document:** avoid overlapping roots).

**3DS `mode=vc` today:** maps to “which single root is active.” Future design should either (a) fold VC exports into `gba_save_dir` (if still GBA), or (b) add explicit `vc_save_dir` as an alias of one of the `*_save_dir` keys — **decide in Phase A** so we don’t ship two conflicting stories.

**Drop:** monolithic `[sync] save_dir` + `[rom] rom_dir` in favor of the above (short-lived compatibility shims optional, not required).

---

## 2. Problem statement

### User constraints

- **Twilight Menu** / **nds-bootstrap** often use layouts like:
  - `.../nds/saves/<title>.sav` next to ROMs, or a centralized saves folder
  - GBA saves under `.../gba/` or `.../gb/` depending on the user
- **Requirement:** GBA and NDS **in one run** — not “sync GBA today, edit config, sync NDS tomorrow.”

### Current product limits

| Area | Today | Blocker |
|------|--------|---------|
| Single `save_dir` | One tree of `*.sav` | Cannot represent disjoint TM paths without a parent + recursion rules. |
| Single `rom_dir` + `rom_extension` | One extension (e.g. `.gba`) | Cannot locate both `.gba` and `.nds` for stem matching. |
| Handheld header code | GBA-only offsets | NDS ROM fed through GBA parser → **wrong or empty** `game_id`; must branch like Python. |
| Bridge | Already multi-system | Clients must **catch up** for parity with Dropbox / `rom_map` workflows. |

---

## 3. Design options

### 3.1 Per-system save + ROM dirs (recommended)

- **Named keys** (§1) — clearer than comma-separated `save_dirs=`.
- **Default scan:** **non-recursive** per `*_save_dir` (Twilight often uses flat `saves/`). Optional later: `save_scan_recursive=true` or per-root flags.
- **Baseline / `.gbasync-baseline` / `.gbasync-idmap`:** either:
  - **A)** one pair of files **per** `*_save_dir` root (simplest mental model), or
  - **B)** a **single** merged file under a chosen “primary” root with keys that include relative path — **pick A or B in Phase A** before coding.

### 3.2 `game_id` resolution (mirror `bridge/game_id.py`)

For each local `.sav`:

1. Determine **system** from which `*_save_dir` matched.
2. Build ROM path: `{*_rom_dir}/{stem}{*_rom_extension}` for that system.
3. Read prefix (512 bytes is enough for GBA and NDS cartridge headers):
   - If extension is **`.nds`** → **NDS** parser: title **12 bytes @ 0x00**, game code **4 bytes @ 0x0C** (see `game_id_from_nds_bytes` in `bridge/game_id.py`).
   - Else if **`.gba`** / **`.gb`** → **GBA** parser @ 0xA0 / 0xAC (current handheld behavior).
   - **GB / GB Color:** separate from GBA; likely **filename-only** or small header stub in a later iteration (§6).

4. Sanitize and join title/code like Python (`title-code` if code present).

### 3.3 Collisions

- Same **stem** in GBA and NDS (`foo.sav` / `foo.gba` vs `foo.nds`) are **different ROMs** — system from save root disambiguates.
- Same stem **within** one system: user error or duplicate ROMs — **document** uniqueness; optional **`nds-` prefix** only if we ever need global uniqueness without headers.

### 3.4 Single parent + recursive scan (advanced, not default)

- One `save_dir` + `**/*.sav` risks picking up stray files; only as power-user option after §3.1 works.

---

## 4. Implementation phases

### Phase A — Spec freeze (docs + decisions, minimal code)

1. Lock **INI keys** (§1) and **3DS `mode=vc` migration story** (fold into `gba_*` vs keep separate key).
2. Choose **baseline strategy** (§3.1 A vs B).
3. **`docs/USER_GUIDE.md`:** Twilight Menu path examples; collision caveats; “NDS requires matching ROM path.”
4. **Test matrix** (manual): GBA-only, NDS-only, mixed, header miss → stem fallback, Delta DS spot-check (no GBA trim).

**Exit criteria:** a reviewer can implement Phase B without reopening schema debates.

### Phase B — Handheld: scan + resolve

1. **3DS + Switch:** parse new config; build union of local saves with **(absolute path, originating root, system enum)**.
2. Implement **`resolve_game_id_for_save`** dispatch by system + extension (share sanitization with existing `sanitize_game_id`).
3. **Baseline / id-map:** multi-root per Phase A decision.
4. **UI / memory:** save viewer and sync loops iterate merged list; watch **3DS** `MAX_SAVES` and heap usage.

### Phase C — Bridge + Delta

1. **Bridge configs:** `rom_dirs` + `rom_extensions` include **`.nds`** alongside **`.gba`** where users store DS ROMs (already supported in Python — validate end-to-end).
2. **Delta / Harmony:** per-game size checks for DS; **no** 131088→131072 trim unless proven identical to a GBA edge case (unlikely).
3. **Tests:** at least one retail `.nds` in Dropbox export regression path.

### Phase D — Docs + release

1. Client READMEs + USER_GUIDE unified workflow.
2. Version bump + release scripts.
3. **Optional:** admin column or badge for “system” metadata if we add `platform` to index later.

---

## 5. Risks and non-goals (MVP)

| Risk | Mitigation |
|------|------------|
| Wrong `game_id` if header missing | Stem fallback (today’s behavior); optional manual map file later. |
| Large DS saves | SHA-256 whole file as today; monitor 3DS list sizes. |
| Delta conflicts | Same as GBA — user resolves in Delta after sync. |

**Out of scope (first ship):** DSiWare / **CIA**-only titles without a loose `.nds`, **RetroArch**-specific layouts unless covered by generic roots, Switch emulators beyond user-configured paths.

---

## 6. Open questions (resolve in Phase A)

1. **GB / GBC:** header-based id vs filename-only; different cartridge format from GBA.
2. **Baseline:** one file per save root vs merged file (§3.1).
3. **Recursive scan:** default **off** per root unless we see a TM layout that requires it everywhere.
4. **Multiple GBA paths on Switch** (e.g. mGBA + another emu): second `gba_save_dir` vs `gba_save_dir_2` — only if users demand before ship.
5. **3DS VC mode:** map `vc_save_dir` to `gba_save_dir` when still GBA VC, or keep explicit `vc_*` until unified schema lands.

---

## 7. Code references (anchors)

| Topic | Location |
|-------|----------|
| NDS + GBA ID (Python) | `bridge/game_id.py` — `game_id_from_nds_bytes`, `game_id_from_gba_bytes`, `game_id_from_rom_bytes`, `_rom_sha1_and_game_id`, `build_rom_sha1_to_game_id` |
| GBA-only handheld | `switch-client/source/main.cpp` — `game_id_from_rom_header`, `resolve_game_id_for_save`, `read_file_prefix` |
| GBA-only handheld | `3ds-client/source/main.c` — `game_id_from_rom_header_bytes`, `resolve_game_id_for_save`, `read_rom_header_prefix` |
| 3DS dual save roots (not multi-system) | `3ds-client/source/main.c` — `active_save_dir`, `mode` / `vc_save_dir` |
| Delta GBA trim | `bridge/delta_dropbox_sav.py` — `apply_bytes_to_delta` |

---

## 8. Effort summary

| Workstream | Rough size | Notes |
|------------|------------|--------|
| Config + multi-dir scan | Medium | Both clients + samples |
| NDS (and optional GB) header in C/C++ | Low–medium | Match Python offsets exactly; unit-style fixtures from real ROM prefixes |
| Baseline / id-map multi-root | Medium | Tied to Phase A choice |
| Bridge / Delta | Low–medium | Mostly validation + DS size logging |
| Server | Low | Blob storage unchanged |

**Bottom line:** Feasible. The **must-have** client work is **multi-root scanning** + **NDS-aware ROM identification** aligned with `bridge/game_id.py`. After that, one config can cover GBA + NDS without swapping profiles.
