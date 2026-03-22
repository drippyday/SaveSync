# Dropbox sync performance plan

This document captures ideas to make **Dropbox-related** GBAsync sync faster and cheaper (API calls, bandwidth, CPU, wall time). It is scoped to the two bridge paths that talk to Dropbox:

| Path | Script | Typical use |
|------|--------|-------------|
| **Delta Harmony (API)** | `bridge/delta_dropbox_api_sync.py` | Server pulls a remote **Delta Emulator** folder via Dropbox HTTP API, runs `TripleSync`, pushes changes back. |
| **Plain `.sav` folder** | `bridge/dropbox_bridge.py` | Flat folder of `.sav` files ↔ GBAsync server. |

Desktop **Delta + local Dropbox app** (folder on disk) uses `delta_folder_server_sync.py` without the API; many of the same **merge** costs apply, but **network** cost is borne by the Dropbox desktop client, not our Python.

---

## Current behavior (baseline)

### Delta API sync (`delta_dropbox_api_sync.py`)

1. **`pull_delta_folder`** lists the remote folder with `files_list_folder(..., recursive=True)` and **downloads every file** on every run into a temp directory.
2. **`TripleSync.sync_once()`** runs on the full tree (merge logic as today).
3. **`_snapshot_files`** hashes **every** local file with SHA-256 (full read) to build the “after merge” baseline for uploads.
4. **`push_changed_files`** walks the tree with `rglob`, compares hashes to the pre-merge snapshot, uploads diffs, then does extra passes for GameSave sidecars / `versionIdentifier` alignment.

**Implication:** Runtime and Dropbox usage scale with **total Harmony folder size** and **file count**, not with “how much changed this pass.” Large libraries make each `--once` cycle heavy.

### Plain bridge (`dropbox_bridge.py`)

1. **`files_list_folder`** (non-recursive) over the configured folder.
2. For each entry, **downloads** the file to compare with the server.

**Implication:** Every sync pass re-downloads remote `.sav` content to compare SHA-256 with the server, even when nothing changed.

### Server-triggered runs (`server/app/main.py`)

- Optional **debounced** subprocess after upload (`GBASYNC_DROPBOX_SYNC_*`).
- Subprocess **timeout** (`GBASYNC_DROPBOX_SYNC_TIMEOUT_SECONDS`, default 600s).

Tuning debounce/timeout affects **how often** sync runs, not per-run efficiency inside the bridge.

---

## Goals

1. **Reduce Dropbox API usage:** fewer `files_download` / `files_upload` calls when data is unchanged.
2. **Reduce bandwidth:** avoid re-downloading large blobs (Harmony `GameSave-*` binaries, ROM blobs) when remote metadata shows no change.
3. **Reduce CPU and I/O:** avoid full-tree SHA-256 passes when a cheaper check suffices; avoid redundant `rglob` work.
4. **Keep correctness:** preserve ordering constraints (e.g. upload blobs before GameSave JSON sidecars), conflict behavior, and lock semantics (`dropbox_run_lock`).

---

## Phased plan

### Phase 1 — Metadata-first incremental pull (Delta API)

**Idea:** Treat `files_list_folder` (and `continue`) as the **authoritative inventory**. Each `FileMetadata` includes `content_hash` (when available) and `rev`. Persist a **per-remote-folder cache** on disk (e.g. under `local_save_dir` or a dedicated cache path): `relative_path → { content_hash or rev, size }`.

**On each sync:**

1. List the remote tree (same recursive listing as today, or a cursor-based continuation — already paginated).
2. For each file, if cache says same `content_hash`/`rev` **and** the local copy exists with matching hash/size, **skip download**.
3. Download only added/changed/missing paths.

**Why it helps:** Pull cost drops from “full mirror every run” to “delta mirror,” which matters most for large Delta libraries.

**Risks / details:**

- First run after upgrade still does a full pull (or explicit `--rebuild-cache`).
- Handle renamed paths (Dropbox may expose as delete + add); cache key should be path-normalized relative to the Delta root.
- Confirm `content_hash` availability for all relevant file types in your Dropbox app scope (app folder vs full Dropbox).

### Phase 2 — Smarter upload detection (Delta API)

**Idea:** Today `_snapshot_files` hashes **everything** after merge. Alternatives:

- Track **which** `TripleSync` paths actually wrote bytes (explicit dirty set from merge).
- Or hash only **patterns that can change** in a pass (`Game-*`, `GameSave-*`, known sidecars) instead of `rglob("*")`.

**Why it helps:** Cuts CPU and disk read on huge trees when only a few games changed.

**Risks:** Must stay conservative: any code path that mutates a file without marking it dirty would miss uploads — needs tests around Harmony write paths.

### Phase 3 — Plain `.sav` bridge: compare before download

**Idea:** After `files_list_folder`, use **`server_modified`** and/or **`content_hash`** from metadata. Maintain a small local cache of `path → hash`. Only **`files_download`** when server metadata says the file changed or cache is cold.

**Why it helps:** Stops re-downloading every `.sav` on every pass when Dropbox and the server already match.

**Risks:** Clock skew is less of an issue if `content_hash` is used; if only timestamps, define a clear comparison rule with server `last_modified_utc`.

### Phase 4 — Parallelism and batching (bounded)

**Idea:** Where downloads/uploads are still required, run a **small** thread pool (or asyncio) with conservative concurrency to respect Dropbox rate limits. Optionally use **`download_zip`** for bulk fetch if the API fits the folder layout (evaluate; Harmony layout may not zip cleanly).

**Why it helps:** Hides latency for many small files.

**Risks:** Rate limiting (`429`), ordering constraints for uploads (blobs before JSON) must still be enforced.

### Phase 5 — Operational tuning (low code)

- **`GBASYNC_DROPBOX_SYNC_DEBOUNCE_SECONDS`:** Avoid hammering API when many uploads arrive at once.
- **`GBASYNC_DROPBOX_SYNC_TIMEOUT_SECONDS`:** Set high enough for a cold full pull; after Phase 1, timeouts can often be lowered.
- **Schedule:** If full pull remains necessary on a timer, less frequent cron reduces average load.

---

## Success metrics

- **Dropbox:** Fewer `files_download` / `files_upload` calls per successful sync when no remote/local changes.
- **Wall time:** p95 duration of `delta_dropbox_api_sync.py --once` on a representative large folder drops materially after Phase 1–2.
- **Correctness:** No increase in Harmony corruption reports; sidecar/blob ordering unchanged.

---

## Out of scope (for this plan)

- **Merge algorithm** inside `TripleSync` / `delta_folder_server_sync.py` (correctness and conflict rules) — performance work should not change outcomes, only I/O shape.
- **Switch/3DS clients** — they do not use these Dropbox scripts.

---

## Implemented (same behavior, less I/O)

These are already in the bridge:

- **`delta_dropbox_api_sync.py`:** **`pull_delta_folder`** lists the tree on the main thread, then downloads with **`ThreadPoolExecutor(max_workers=2)`**; each worker uses its own **`make_dropbox_client()`** (SDK sessions are not shared across threads). Downloads use **`files_download_to_file`** so bytes stream to disk instead of buffering **`res.content`**. **`push_changed_files`:** sidecars use chunked **`_sha256_file`** for the pre-merge comparison only; non-sidecars use **`_sha256_and_bytes`** (one pass, same digest + payload for upload). One sorted file list for change detection and sidecar enumeration.
- **`dropbox_bridge.py`:** **`sync_once`** reuses the first **`_list_dropbox_savs()`** snapshot for the second phase — the first phase only **PUT**s to the server and does not modify Dropbox, so re-downloading every `.sav` was redundant.

---

## References (code)

- `bridge/delta_dropbox_api_sync.py` — `pull_delta_folder`, `_snapshot_files`, `push_changed_files`, `main()` temp-dir workflow.
- `bridge/dropbox_bridge.py` — `DropboxServerBridge._list_dropbox_savs`, `sync_once`.
- `server/app/main.py` — `_run_dropbox_bridge_once`, debounce and timeout env vars.
