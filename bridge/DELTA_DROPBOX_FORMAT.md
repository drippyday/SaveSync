# Delta Dropbox export layout (Harmony) ↔ plain `.sav`

This describes what you get when you download **“Delta Emulator”** from Dropbox (folder + files). It is **reverse-engineered** from user exports, not an official spec—test on a copy before overwriting anything Delta still syncs.

## What each thing is

| Pattern | Role |
|--------|------|
| `Game-{id}` | Small **JSON** (no extra suffix). `id` = 40 hex chars, usually **SHA-1 of the `Game-{id}-game` ROM bytes**. Contains `name`, ROM `sha1Hash`, etc. In some exports **`sha1Hash` ≠ SHA-1(blob)** even though `id` matches the blob digest—treat the **`-game` file** as authoritative for ROM identity. |
| `Game-{id}-game` | **ROM** binary (not a save). |
| `GameSave-{id}` or `gamesave-{id}` | Small **JSON** sidecar for the save record: `record.sha1`, `record.modifiedDate`, `files[]` with `size`, `remoteIdentifier`, `versionIdentifier`. |
| `GameSave-{id}-gameSave` or Dropbox path `gamesave-{id}-gamesave` | **Raw save RAM** (same bytes you’d put in a `.sav` for many emulators). Sizes in samples: 32 768, 131 072, 262 144, 524 288 bytes (system-dependent). |

`Game` and `GameSave` share the same **`identifier`** (`id`) for a given title— that’s how they’re linked.

The internet workflow (“search for the number string, pick latest GameSave, rename to `.sav`”) matches **`GameSave-{id}-gameSave`** (or the lowercase `gamesave-…-gamesave` name Dropbox shows in `remoteIdentifier`).

## Can we convert?

| Direction | Feasible? | Notes |
|-----------|-----------|--------|
| **Delta → `.sav`** | **Yes** | Copy the `*-gameSave` / `*-gamesave` file bytes; name the file from `Game-{id}` → `name` or ROM id. |
| **`.sav` → Delta** | **Mostly yes** | Overwrite the same blob file. Update the **GameSave** JSON: `record.sha1`, `record.modifiedDate`, and `files[0].sha1Hash` (all SHA-1 of the **save file bytes**). Keep **`files[0].remoteIdentifier`** consistent with the blob filename Delta will fetch (often a Dropbox path whose **basename** is `gamesave-…-gamesave` while another copy on disk is `GameSave-…-gameSave`); mismatch → download vs hash crash on launch. For Dropbox API sync, sidecar `files[0].versionIdentifier` should match the blob's Dropbox `rev`. |
| **Harmony “API”** | **No** | There is no public library; we only edit files Dropbox syncs. |

## Risks

- **Do not rewrite `Game-{id}` JSON** (ROM `sha1Hash`, `versionIdentifier`, etc.) to “match” the `-game` blob unless you know what you’re doing. Delta/Harmony treats those records as part of its sync contract; changing them via Dropbox caused **“conflict with this record”** and **DownloadError** in practice. GBAsync only patches **`GameSave-*`** sidecars when writing save bytes (`apply_bytes_to_delta`).

## “Conflict with this record” (every game)

Harmony compares the **Dropbox copy** of each `GameSave-*` JSON + `*-gameSave` blob with what the **Delta app** last had in its local database. After GBAsync **overwrites** those files in Dropbox (server or 3DS won the merge), Delta often shows a conflict until you pick a side — that can look like **all titles at once** if one sync pass touched every save.

**What to do**

1. **Prefer the cloud / Dropbox copy** (wording varies by Delta version) so the device matches what GBAsync just uploaded. If there is a per-game chooser, repeat for each, or use any bulk “restore from cloud” style option if offered.
2. **Avoid editing saves on the phone** while the Docker sidecar is pushing (30s interval); concurrent local + API edits make conflicts more likely.
3. If conflicts persist after a rebuild: confirm you did **not** hand-edit any **`Game-{id}`** JSON (only `GameSave-*` should change). Re-download the Delta Emulator folder from Dropbox into a **fresh** app data path only as a last resort (back up first).

GBAsync’s `apply_bytes_to_delta` rewrites `remoteIdentifier` so its basename matches the primary blob being updated (keeping a leading folder prefix if present) and materializes that path if it was missing locally—so the attachment Delta downloads matches `sha1Hash`. In Dropbox API mode, `delta_dropbox_api_sync.py` then aligns sidecar `versionIdentifier` to the uploaded blob's Dropbox `rev`.

**Timestamp tug-of-war:** `server_delta` merge picks the newer of GBAsync’s `server_updated_at` and Delta’s `record.modifiedDate`. Resolving conflicts in Delta often **bumps** `modifiedDate` to “now”, so Dropbox can look **newer** than a fresh 3DS upload and the bridge will skip pushing server bytes to Dropbox—or even PUT Delta back onto the server.

For two-way mode (`SAVESYNC_SERVER_DELTA_ONE_WAY=false`), use guardrails:

- `SAVESYNC_SERVER_DELTA_MIN_DELTA_WIN_SECONDS=900`
- `SAVESYNC_SERVER_DELTA_RECENT_SERVER_PROTECT_SECONDS=3600`

This keeps two-way sync while preventing immediate flip-backs after opening Delta.

**Crash on launch:** If both ``GameSave-{id}`` **and** ``gamesave-{id}`` JSON exist (or multiple ``*-gameSave`` spellings), updating **only one** leaves inconsistent metadata vs the RAM blob. GBAsync’s ``apply_bytes_to_delta`` now writes **all** matching blob paths and **all** JSON sidecars with the same content. If Delta still crashes, restore the **Delta Emulator** folder from Dropbox version history to before the bad sync, or remove a corrupted game’s ``GameSave-*`` / ``gamesave-*`` pair (e.g. a hack that previously received the wrong ROM’s save bytes).
- **Wrong `id`**: Importing into the wrong `GameSave-*` corrupts another game.
- **Wrong size**: Truncation or oversized writes may confuse Delta until you fix length + JSON `size`.
- **Unchanged fields**: If Delta validates more than SHA1/date/version, edits could be rejected or repaired—keep backups.
- **Case paths**: Dropbox may show `gamesave-…` vs `GameSave-…`; the tool checks several spellings.

## Tooling

See **`delta_dropbox_sav.py`** (`list` / `export` / `import`).

To keep the **GBAsync server** and this **Delta Dropbox folder** aligned — including **rewriting Harmony blobs** from the newest **device/server** save — use **`delta_folder_server_sync.py`** with **`sync_mode`: `server_delta`** or **`triple`** (see **`README.md`**, **`config.example.delta_sync.json`**).
