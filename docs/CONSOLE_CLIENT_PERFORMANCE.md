# Console client performance (Switch & 3DS)

This document describes **where time and memory go** in the GBAsync **Switch** and **3DS** homebrew clients, and lists **improvements that are “safe” in principle** (same sync semantics, same server API usage) versus changes that need extra care.

It is **not** a commitment to implement any item; it is a roadmap for profiling-driven work.

---

## What dominates today (both clients)

1. **Network: one TCP connection per HTTP request**  
   Both implementations open a socket, send `Connection: close`, transfer the body, then close (`switch-client/source/main.cpp` and `3ds-client/source/main.c` `http_request`). A full **Auto** sync can issue **many** sequential requests (`GET /saves`, many `GET`/`PUT`/`PATCH`, history calls, etc.). **RTT × request count** often dominates wall time on Wi‑Fi.

2. **SHA-256 over full save files**  
   Local scans read each `.sav` (and hash it) to compare with the server and baselines. Large saves mean large reads and CPU time.

3. **Directory scans**  
   Multi-root configs (GBA / NDS / GB) walk more trees; work grows with file count.

4. **JSON handling**  
   `GET /saves` returns one blob; clients parse it (custom string walks / small parsers). Usually secondary to network + hashing unless the index is huge.

---

## Safe or low-risk directions

### 1. HTTP keep-alive (or session-scoped connection) — **high leverage, medium effort**

**Idea:** For a **single sync run**, reuse one TCP connection to the same host/port for multiple requests (HTTP/1.1 `Connection: keep-alive`, read response until `Content-Length` or chunked complete, then send the next request on the same socket).

**Why it’s “safe”:** Same requests, same headers, same bodies; only the transport is reused. **Caveats:** Must correctly parse **each** response (length, chunked body) before reusing the socket; must handle server `Connection: close` and fall back to a new socket.

**Why not done yet:** The current helper is intentionally minimal (easy to audit). Keep-alive is a mechanical upgrade with more edge cases.

### 2. Streaming SHA-256 (chunked file read) — **safe, moderate RAM win**

**Idea:** Hash files in fixed-size chunks instead of loading the entire save into a buffer, then hashing (`read_file` + `hash` pattern).

**Why it’s safe:** Same SHA-256 result as buffering the whole file; only memory layout changes.

**Limits:** Peak RAM drops; **wall time** may improve slightly on memory‑starved devices; **won’t** cut bytes read from SD.

### 3. “Redundant” work — what we verified in code

This section replaces a vague “profile it” note with **what is and isn’t redundant** in the current Switch/3DS sources.

#### `GET /saves` — **not** duplicated in the core sync path

- **Switch** (`sync_with_server_work` around `main.cpp` where `scan_all_local_saves` then `GET /saves`): **one** `GET /saves` per sync invocation.
- **3DS** (main Auto/upload/download path around `scan_all_local_saves` then `http_request` … `GET /saves`): **one** `GET /saves` per sync invocation.

**Other** `GET /saves` calls belong to **different entry points** (e.g. manual download picker, save viewer) — **not** a second fetch inside the same Auto run.

**Conclusion:** Caching `GET /saves` **inside a single sync** is unnecessary because there is **no second** fetch in that path. **Do not** add a cache that hides a **fresh** server list after a long interactive pause unless you define that explicitly.

#### Local scan + hashing — **each path once per scan**

In `scan_local_saves_in_dir` / `scan_all_local_saves`, each `.sav` is read and hashed **once** per scan. Deduping (`dedupe_local_saves_by_path_keep_last` on Switch) avoids duplicate rows for the same path.

#### Upload path — **double disk read** (real redundancy, **safe** to fix carefully)

**Switch** `put_save_log` reads `l.path` with `read_file` for the PUT body **after** `scan_all_local_saves` already read and hashed the same file into `l.sha256`.

**3DS** `put_one_save` does the same: `read_file_bytes(l->path, …)` after the scan filled `l->sha256`.

So **every upload** pays **two full reads** of the same file (scan + PUT). The PUT **must** send the current bytes; the scan already proved the digest. A **safe** optimization is one read in the upload helper: stream/hash or read once, verify digest matches `l.sha256` (or recompute and compare), then PUT. **If** the file differs from the scan (rare race), handle like today: upload the **new** bytes.

**Conclusion:** This is the main **redundant I/O** worth targeting without changing sync semantics.

#### 3DS `scan_all_local_saves` called again after download / Auto

**3DS** calls `scan_all_local_saves` again after **download-only** (refresh locals after `get_one_save`) and after **Auto** completes (before baseline save). **Files on disk changed** in between; **not** redundant with the first scan.

**Conclusion:** **Do not** skip these rescans with a “session cache” of the first scan.

### 4. Smaller / cheaper logging on hot paths — **safe, small win**

**Idea:** Reduce `printf` / `consoleUpdate` churn during tight loops if profiling shows it matters.

**Why it’s safe:** Cosmetic; keep user-visible error paths.

### 5. Operational (no code)

- Strong **Wi‑Fi** and a **low-latency** path to the server (LAN vs internet) matter more than micro-optimizations in C/C++.
- **Lock** games you are not syncing to shrink Auto plans (`locked_ids` / equivalent).
- **Fewer roots** in `config.ini` (only the systems you use) reduces scan work.

---

## Higher risk (needs design + tests)

### Parallel HTTP uploads

Could overlap network waits, but **ordering**, **memory**, and **error handling** get harder on embedded. Not “free” speed.

### Skipping re-hash using mtime/size vs baseline

Can avoid full reads when **nothing** changed, but **wrong** skip ⇒ wrong sync decisions if clocks or copies are misleading. Would need a clear policy and tests.

### Changing conflict / ordering rules

Anything that alters **which** bytes win is **not** a performance-only change.

---

## Platform-specific notes

### Switch (`switch-client/source/main.cpp`)

- Custom `http_request` + `parse_saves_json` substring parser: **CPU** is usually fine; **per-request TCP** is the main network cost.
- **ROM header reads** use a **prefix** read (e.g. 512 bytes) for NDS/GBA — already avoids reading full ROMs for ID.

### 3DS (`3ds-client/source/main.c`)

- Similar **HTTP** pattern (`Connection: close`).
- Tight **heap** limits in places; **streaming hash** may help **stability** on large saves as much as speed.

---

## How to validate improvements

1. **Time** a representative Auto sync (cold vs warm Wi‑Fi).
2. **Count** HTTP requests per run (or log in server access logs).
3. **Compare** SHA-256 of a few saves before/after any hashing refactor (must match bit-for-bit).

---

## Related docs

- **`docs/DROPBOX_SYNC_PERFORMANCE_PLAN.md`** — Dropbox **API** bridge (server-side), not the console HTTP clients.
- **`switch-client/README.md`**, **`3ds-client/README.md`** — config and behavior.

---

## Release builds

After **editing** `switch-client/` or `3ds-client/`, rebuild with devkitPro from the repo root:

```bash
./scripts/release-switch.sh dev
./scripts/release-3ds.sh dev
```

See **`.cursor/rules/rebuild-console-clients.mdc`**.
