#!/usr/bin/env python3
"""
GBAsync server ↔ Delta Emulator **via Dropbox HTTP API** (no desktop sync client).

Downloads the remote **Harmony** folder (``Game-*``, ``GameSave-*``, ``*-gameSave``) to a
temporary directory, runs the same merge as ``delta_folder_server_sync.py`` (``triple`` or
``server_delta``), then uploads any changed files back. Uses the official Dropbox SDK
(https://www.dropbox.com/developers/documentation/http/documentation).

Credentials: repository-root ``.env`` — see ``DROPBOX_SETUP.md``.

Config: ``config.example.delta_dropbox_api.json`` — set ``dropbox.remote_delta_folder`` to the
API path of your Delta folder (e.g. app-folder ``/Delta Emulator`` or full Dropbox path).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
from collections.abc import Callable
from pathlib import Path

try:
    import dropbox
    from dropbox.files import FileMetadata, WriteMode
    from dropbox.exceptions import ApiError
except ImportError as exc:  # pragma: no cover
    raise SystemExit("Install Dropbox deps: pip install -r requirements-dropbox.txt") from exc

from delta_folder_server_sync import TripleConfig, TripleSync
from dropbox_env import make_dropbox_client
from dropbox_run_lock import dropbox_run_lock


def _norm_remote_folder(s: str) -> str:
    s = s.strip()
    if not s.startswith("/"):
        s = "/" + s
    return s.rstrip("/")


def _remote_rel_path(remote_folder: str, path_display: str) -> str | None:
    rf = remote_folder.rstrip("/")
    pd = path_display
    pref = rf + "/"
    if len(pd) <= len(pref) or pd[: len(pref)].lower() != pref.lower():
        return None
    return pd[len(pref) :]


_CHUNK = 1024 * 1024


def _sha256_file(path: Path) -> str:
    """SHA-256 of file contents (same digest as ``hashlib.sha256(read_bytes())``)."""
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            b = f.read(_CHUNK)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _sha256_and_bytes(path: Path) -> tuple[str, bytes]:
    """One pass over disk: digest + full bytes (for uploads). Same SHA-256 as ``_sha256_file``."""
    h = hashlib.sha256()
    buf = bytearray()
    with path.open("rb") as f:
        while True:
            b = f.read(_CHUNK)
            if not b:
                break
            h.update(b)
            buf.extend(b)
    return h.hexdigest(), bytes(buf)


def _snapshot_files(root: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        out[rel] = _sha256_file(p)
    return out


_GAMESAVE_BLOB_RE = re.compile(r"^gamesave-([0-9a-f]{40})-gamesave$", re.I)
_GAMESAVE_JSON_RE = re.compile(r"^gamesave-([0-9a-f]{40})$", re.I)


def _gamesave_blob_identifier(path: Path) -> str | None:
    m = _GAMESAVE_BLOB_RE.fullmatch(path.name)
    return m.group(1).lower() if m else None


def _gamesave_json_identifier(path: Path) -> str | None:
    m = _GAMESAVE_JSON_RE.fullmatch(path.name)
    return m.group(1).lower() if m else None


def _sidecar_blob_remote_path(sidecar: Path, remote_folder: str) -> str | None:
    try:
        j = json.loads(sidecar.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, UnicodeError):
        return None
    files = j.get("files")
    if not isinstance(files, list) or not files or not isinstance(files[0], dict):
        return None
    ri = files[0].get("remoteIdentifier")
    if not isinstance(ri, str) or not ri.strip():
        return None
    ri_norm = ri.strip().replace("\\", "/")
    if ri_norm.startswith("/"):
        return ri_norm
    return f"{_norm_remote_folder(remote_folder)}/{ri_norm.lstrip('/')}"


def _set_sidecar_version_identifier(sidecar: Path, version_identifier: str) -> bool:
    try:
        j = json.loads(sidecar.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, UnicodeError):
        return False
    files = j.get("files")
    if not isinstance(files, list) or not files or not isinstance(files[0], dict):
        return False
    f0 = files[0]
    if str(f0.get("versionIdentifier") or "").strip() == version_identifier:
        return False
    f0["versionIdentifier"] = version_identifier
    sidecar.write_text(json.dumps(j, separators=(",", ":"), ensure_ascii=False), encoding="utf-8")
    return True


def pull_delta_folder(dbx: dropbox.Dropbox, remote_folder: str, dest: Path) -> None:
    """List the remote tree, then download with two workers (separate Dropbox clients per thread).

    ``files_download_to_file`` streams to disk; ``ThreadPoolExecutor(2)`` overlaps network waits.
    """
    remote_folder = _norm_remote_folder(remote_folder)
    dest.mkdir(parents=True, exist_ok=True)
    jobs: list[tuple[str, Path]] = []
    result = dbx.files_list_folder(remote_folder, recursive=True)
    while True:
        for ent in result.entries:
            if not isinstance(ent, FileMetadata):
                continue
            rel = _remote_rel_path(remote_folder, ent.path_display)
            if not rel:
                continue
            local = dest / rel
            jobs.append((ent.path_display, local))
        if not result.has_more:
            break
        result = dbx.files_list_folder_continue(result.cursor)

    if not jobs:
        return

    thread_local = threading.local()

    def _thread_dbx() -> dropbox.Dropbox:
        if getattr(thread_local, "client", None) is None:
            thread_local.client = make_dropbox_client()
        return thread_local.client

    def _download_one(job: tuple[str, Path]) -> None:
        path_display, local = job
        local.parent.mkdir(parents=True, exist_ok=True)
        # download_path = local disk, path = Dropbox (SDK parameter names).
        _thread_dbx().files_download_to_file(str(local), path_display)

    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(_download_one, jobs))


def push_changed_files(
    dbx: dropbox.Dropbox,
    remote_folder: str,
    local_root: Path,
    before: dict[str, str],
    dry_run: bool,
    log: Callable[[str], None],
) -> None:
    remote_folder = _norm_remote_folder(remote_folder)

    def _is_gamesave_json_sidecar(path: Path) -> bool:
        n = path.name
        if not (n.startswith("GameSave-") or n.startswith("gamesave-")):
            return False
        return not n.endswith("-gameSave") and not n.endswith("-gamesave")

    files_sorted = sorted(
        (p for p in local_root.rglob("*") if p.is_file()),
        key=str,
    )

    changed: list[Path] = []
    # Bytes for changed non-sidecars only — one chunked read via _sha256_and_bytes (sidecars may be
    # rewritten by _set_sidecar_version_identifier before final upload, so those stay read fresh).
    blob_payload: dict[Path, bytes] = {}
    for p in files_sorted:
        rel = p.relative_to(local_root).as_posix()
        if _is_gamesave_json_sidecar(p):
            h = _sha256_file(p)
            if before.get(rel) == h:
                continue
            changed.append(p)
        else:
            h, data = _sha256_and_bytes(p)
            if before.get(rel) == h:
                continue
            changed.append(p)
            blob_payload[p] = data

    # Upload blobs first, JSON sidecars last. This narrows the window where Delta could read a
    # freshly-uploaded GameSave JSON whose referenced blob has not arrived yet.
    changed.sort(key=lambda p: (1 if _is_gamesave_json_sidecar(p) else 0, str(p)))

    changed_sidecars: list[Path] = [p for p in changed if _is_gamesave_json_sidecar(p)]
    changed_non_sidecars: list[Path] = [p for p in changed if not _is_gamesave_json_sidecar(p)]
    blob_revs_by_gid: dict[str, str] = {}

    for p in changed_non_sidecars:
        rel = p.relative_to(local_root).as_posix()
        data = blob_payload[p]
        remote_path = f"{remote_folder}/{rel}"
        if dry_run:
            log(f"[dry-run] upload {remote_path} ({len(data)} B)")
            continue
        md = dbx.files_upload(data, remote_path, mode=WriteMode.overwrite)
        gid = _gamesave_blob_identifier(p)
        if gid and isinstance(md, FileMetadata):
            blob_revs_by_gid[gid] = md.rev
        log(f"[dropbox] upload {remote_path} ({len(data)} B)")

    sidecars_to_upload: dict[str, Path] = {str(p): p for p in changed_sidecars}
    all_sidecars: list[Path] = [p for p in files_sorted if _is_gamesave_json_sidecar(p)]

    for sidecar in all_sidecars:
        gid = _gamesave_json_identifier(sidecar)
        if not gid:
            continue
        rev = blob_revs_by_gid.get(gid)
        if not rev and not dry_run:
            blob_remote_path = _sidecar_blob_remote_path(sidecar, remote_folder)
            if blob_remote_path:
                try:
                    md = dbx.files_get_metadata(blob_remote_path)
                except ApiError:
                    md = None
                if isinstance(md, FileMetadata):
                    rev = md.rev
        if not rev:
            continue
        if _set_sidecar_version_identifier(sidecar, rev):
            sidecars_to_upload[str(sidecar)] = sidecar
            log(f"[dropbox] align versionIdentifier {sidecar.name} -> {rev}")

    for p in sorted(sidecars_to_upload.values(), key=lambda x: str(x)):
        rel = p.relative_to(local_root).as_posix()
        data = p.read_bytes()
        remote_path = f"{remote_folder}/{rel}"
        if dry_run:
            log(f"[dry-run] upload {remote_path} ({len(data)} B)")
            continue
        dbx.files_upload(data, remote_path, mode=WriteMode.overwrite)
        log(f"[dropbox] upload {remote_path} ({len(data)} B)")


def load_api_config(path: Path) -> tuple[TripleConfig, str]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    dbx = raw.get("dropbox") or {}
    remote = str(dbx.get("remote_delta_folder", "")).strip()
    if not remote:
        raise ValueError("config missing dropbox.remote_delta_folder (Dropbox API path to Delta folder)")
    remote = _norm_remote_folder(remote)

    rom_dirs = [Path(p).expanduser() for p in raw.get("rom_dirs", [])]
    rmp = Path(raw["rom_map_path"]).expanduser() if raw.get("rom_map_path") else None
    mode = str(raw.get("sync_mode", "triple")).strip().lower()
    if mode not in ("triple", "server_delta"):
        raise ValueError('sync_mode must be "triple" or "server_delta"')

    ow = raw.get("server_delta_one_way", False)
    if isinstance(ow, str):
        ow = ow.strip().lower() in ("1", "true", "yes")
    else:
        ow = bool(ow)
    placeholder = TripleConfig(
        server_url=raw["server_url"].rstrip("/"),
        api_key=raw.get("api_key", ""),
        local_save_dir=Path(raw["local_save_dir"]).expanduser(),
        delta_root=Path("/nonexistent-delta-placeholder"),
        rom_dirs=rom_dirs,
        rom_map_path=rmp,
        delta_slot_map_path=Path(raw["delta_slot_map_path"]).expanduser()
        if raw.get("delta_slot_map_path")
        else None,
        rom_extensions=raw.get("rom_extensions", [".gba"]),
        backup_dir=Path(raw["backup_dir"]).expanduser() if raw.get("backup_dir") else None,
        sync_mode=mode,
        server_delta_one_way=ow,
    )
    return placeholder, remote


def main() -> None:
    here = str(Path(__file__).resolve().parent)
    if here not in sys.path:
        sys.path.insert(0, here)

    p = argparse.ArgumentParser(
        description="GBAsync ↔ Delta Harmony folder via Dropbox API (HTTP/SDK)"
    )
    p.add_argument("--config", type=Path, required=True)
    p.add_argument("--once", action="store_true", help="single pass (default)")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if not args.once:
        raise SystemExit("Only --once is implemented (use cron or a loop externally).")

    cfg_base, remote_folder = load_api_config(args.config.expanduser().resolve())
    cfg_base.local_save_dir.mkdir(parents=True, exist_ok=True)

    dbx = make_dropbox_client()

    def log(msg: str) -> None:
        print(msg, flush=True)

    with dropbox_run_lock(), tempfile.TemporaryDirectory(prefix="gbasync-delta-dbx-") as tmp:
        tmp_path = Path(tmp)
        log(f"[dropbox] pull {remote_folder} -> {tmp_path}")
        try:
            pull_delta_folder(dbx, remote_folder, tmp_path)
        except ApiError as exc:
            raise SystemExit(f"Dropbox pull failed: {exc}") from exc

        before = _snapshot_files(tmp_path)
        cfg = cfg_base.with_delta_root(tmp_path)
        if not cfg.delta_root.is_dir():
            raise SystemExit("Temp delta root missing after pull")

        if args.dry_run:
            log("[dry-run] merge (no server/Delta writes); uploads logged only")
        TripleSync(cfg, dry_run=args.dry_run).sync_once()

        try:
            push_changed_files(dbx, remote_folder, tmp_path, before, args.dry_run, log)
        except ApiError as exc:
            raise SystemExit(f"Dropbox upload failed: {exc}") from exc

    log("[done] Delta Dropbox API sync complete")


if __name__ == "__main__":
    main()
