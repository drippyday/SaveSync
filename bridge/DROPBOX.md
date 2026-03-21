# Dropbox ↔ GBAsync (optional)

**Quick setup (`.env` + config JSON):** see **[`DROPBOX_SETUP.md`](DROPBOX_SETUP.md)**.

## What this is *not*

- **Not** a replacement for the Delta **app’s** own sync UI. We only read/write **files** Dropbox stores (via the [HTTP / `files/*` API](https://www.dropbox.com/developers/documentation/http/documentation) or a local synced folder).
- **`dropbox_bridge.py` is not for Harmony.** It targets a **flat** folder of `*.sav` files you control, not Delta’s `GameSave-*` layout.

## What this is

`dropbox_bridge.py` syncs a **Dropbox folder** full of plain `*.sav` files with your **GBAsync server** (same HTTP API as Switch/3DS). It uses the same **game_id** rules as `bridge.py` (ROM map / ROM dirs / filename stem).

## Delta Emulator (Harmony) over the Dropbox API

If you **do not** use the Dropbox desktop app but still want GBAsync to merge with Delta’s real cloud files, use **`delta_dropbox_api_sync.py`**:

1. Same credentials as **`DROPBOX_SETUP.md`**: usually **`DROPBOX_ACCESS_TOKEN`** (App Console → Generate) or key + secret + refresh token (scopes: `files.metadata.read`, `files.content.read`, `files.content.write`).
2. Copy **`config.example.delta_dropbox_api.json`** → `config.delta_dropbox_api.json`.
3. Set **`dropbox.remote_delta_folder`** to the API path of the folder that contains `Game-*`, `GameSave-*`, and `*-gameSave` (for an **app-folder** app this is often `/Delta Emulator` or similar—check the path Dropbox shows for your app).
4. `pip install -r requirements-dropbox.txt` then:

   ```bash
   python3 delta_dropbox_api_sync.py --config config.delta_dropbox_api.json --once
   ```

Each run **downloads** that tree to a temp directory, runs the same logic as **`delta_folder_server_sync.py`** (including rewriting Harmony blobs when the server wins), then **uploads only changed files**. Large folders mean more bandwidth than editing a local sync directory.

## Compare to `bridge.py`

| Tool | Watches |
|------|--------|
| **`bridge.py`** | A **local** directory (`delta_save_dir`), e.g. Delta’s save folder on Mac when it maps to disk. |
| **`dropbox_bridge.py`** | A **Dropbox API** path of plain `*.sav` files, e.g. `/GBAsync/gba`. |
| **`delta_dropbox_api_sync.py`** | A **Dropbox API** path to the **Delta Emulator / Harmony** folder (recursive download/upload). |

## Setup

1. **Create a Dropbox app** (Dropbox Developers → Create app).  
   - Scoped access → **App folder** (recommended) or full Dropbox.  
   - Note **App key** and **App secret**.

2. **OAuth refresh token** (one-time; Dropbox documents the PKCE / refresh-token flow).  
   Common approach: use Dropbox’s **OAuth guide** or a small token helper to obtain `DROPBOX_REFRESH_TOKEN` with scopes `files.metadata.read`, `files.content.read`, `files.content.write`.

3. **Credentials** — use the **repository-root** **`.env`** (see **`DROPBOX_SETUP.md`**) or `export` the same variable names in the shell.

4. **Install deps**:

   ```bash
   cd bridge
   pip install -r requirements-dropbox.txt
   ```

5. **Config**: copy `config.example.dropbox.json` → `config.dropbox.json`, set `server_url`, `api_key`, and `dropbox.remote_folder` (must exist in Dropbox; create the folder in the Dropbox UI first).

6. **Run** (from the `bridge/` directory so `game_id` imports resolve):

   ```bash
   cd bridge
   python3 dropbox_bridge.py --config config.dropbox.json --once
   # or
   python3 dropbox_bridge.py --config config.dropbox.json --watch
   ```

## Limitations (MVP)

- Only non-recursive listing of **one** `remote_folder` (no subfolders).
- Downloads **every** `.sav` each pass to compare hashes (fine for small sets).
- Timestamp merge is string-compare ISO timestamps (same style as local `bridge.py`).

## Workflow idea with consoles

1. Copy `.sav` from Switch/SD into your Dropbox folder (or automate with another tool).  
2. Run `dropbox_bridge.py --watch` on a always-on machine.  
3. GBAsync server stays the hub; Switch/3DS use the normal homebrew clients.

---

## Harmony, hash-style names, and “just replace the save on Dropbox”

### Can GBAsync use Harmony?

**Not as a library.** [Harmony](https://github.com/rileytestut/DeltaCore/wiki/Syncing) is Delta’s **internal** sync layer (Core Data + cloud backends). There is no public Python API to “register” a save. You **can** still update the **same files** Delta syncs—`GameSave-*` JSON and `*-gameSave` blobs—either on disk (`delta_folder_server_sync.py`) or by download/edit/upload through the [Dropbox HTTP API](https://www.dropbox.com/developers/documentation/http/documentation) (`delta_dropbox_api_sync.py`). That is **file-level** editing, not a supported contract from the Delta team; test on copies first.

### Why are Dropbox files named with hashes / not `something.gba`?

Those names are **opaque Harmony record IDs** (or similar), not “the filename of your `.sav` on SD.” The real save bytes and metadata live in a structure Delta owns. Random-looking names are normal.

### Your goal: “newer save from another device → phone, or pull from Dropbox if newer”

**Fully automatic, by overwriting files inside Delta’s Dropbox folder, is not something GBAsync can promise.**

- If you **upload a random `.sav`** next to Harmony’s files (or rename one to look like a hash), Delta may **ignore it**, **conflict**, or worse — behavior is undefined because the format is private.
- **What does work today** is a **hub model**: **GBAsync server** holds the canonical save; **Switch / 3DS** sync against the server with the homebrew apps; **Delta on iPhone** is updated only through paths Delta actually supports, for example:
  - **Manual** export/import or save-manager workflows if Delta exposes them, or  
  - **`bridge.py` on a Mac** if Delta (or iOS backup tooling) gives you a **real folder of `.sav` files** on disk — then the bridge can push/pull vs the server on a schedule. That still may not be the same folder as “what you see in the Dropbox website for Delta.”

**`dropbox_bridge.py`** is for a **separate** Dropbox directory of **plain `*.sav` files** *you* maintain. It can keep that folder in sync with the GBAsync server (newer-wins style, same idea as the desktop bridge). It does **not** push into Harmony’s layout, so it will **not** by itself make Delta on the phone pick up a new save unless you separately get that `.sav` into Delta through a supported channel.

### Practical summary

| Approach | Phone updates automatically? |
|----------|-----------------------------|
| Edit Harmony files in Delta’s Dropbox (local or API), with correct JSON + padding | **Often works** in practice when Delta next syncs; **not** officially guaranteed — use backups. |
| GBAsync server + console clients | **Yes** for Switch/3DS. |
| Same server + `delta_folder_server_sync.py` (local Dropbox-synced folder) | **Yes** for that tree ↔ server after Dropbox mobile sync. |
| Same server + `delta_dropbox_api_sync.py` (Dropbox API, no desktop app) | **Same**, after API upload and Delta pulls from Dropbox. |
| Same server + `bridge.py` on a machine that sees **real** `.sav` files Delta uses | **Sometimes** on Mac/desktop if such a path exists. |
| Same server + `dropbox_bridge.py` on **your** plain `.sav` folder | **Yes** for that folder ↔ server; **no** Harmony layout. |
| Manual import into Delta after downloading a `.sav` | **Yes**, but manual. |

If the product goal is “one automatic loop: consoles ↔ phone with no manual step,” that needs **cooperation from Delta** (e.g. official sync target, URL scheme, or documented save location) — not something GBAsync can complete alone.

### Update: plain `.sav` inside the Dropbox export *does* exist

The files named like `GameSave-{id}-gameSave` (or `gamesave-{id}-gamesave`) are **raw save RAM** plus small JSON sidecars. We document that layout in **`DELTA_DROPBOX_FORMAT.md`** and ship **`delta_dropbox_sav.py`** to **export** those blobs to `.sav` and **import** a `.sav` back (patching JSON checksums). That is **not** using Harmony as an API—it is **editing the same files** Delta syncs. Always work on a **copy** and re-upload to Dropbox so Delta can pull changes.
