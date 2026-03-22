#!/usr/bin/env python3
"""
Sync GBAsync server ↔ Delta Emulator Dropbox folder (Harmony files on disk).

**``sync_mode``: ``triple`` (default)** — three-way merge: local ``*.sav`` folder, server
(device uploads), and Delta's ``GameSave-*`` blobs. Newest timestamp wins; winners push
to the other sides. Local files are also converted **into** Delta's format via
``apply_bytes_to_delta`` when local or server wins.

**``sync_mode``: ``server_delta``** — no local mtime in the merge: only **server vs Delta**.
Use this when saves from 3DS/Switch hit the server and you want the **newest device/server
copy written into Delta's Dropbox-synced folder** (and the other way when Delta is newer).
ROMs in ``rom_dirs`` / ``rom_map_path`` must match Delta's ``sha1Hash`` so each Delta game
maps to the correct ``game_id``. ``local_save_dir`` is still used as an optional **mirror**
of the winning bytes (e.g. ``save_data/saves``).

This is **not** ``dropbox_bridge.py`` (flat ``.sav`` in Dropbox API). Delta layout is
``GameSave-*-gameSave`` + JSON; we edit those files locally so the Dropbox app uploads them.

Examples:
  python3 delta_folder_server_sync.py --config config.delta_sync.json --once
  python3 delta_folder_server_sync.py --config config.delta_sync.json --dry-run --once
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from collections.abc import Callable
from typing import Any

import requests

_BRIDGE_DIR = Path(__file__).resolve().parent
if str(_BRIDGE_DIR) not in __import__("sys").path:
    __import__("sys").path.insert(0, str(_BRIDGE_DIR))


def _load_bridge_module():
    """Load ``bridge.py`` under a stable name; must pre-register in ``sys.modules`` for dataclasses."""
    path = _BRIDGE_DIR / "bridge.py"
    name = "savesync_bridge_impl"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load bridge.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


_ssb = _load_bridge_module()
utc_iso_for_local_sav = _ssb.utc_iso_for_local_sav
sha256_bytes = _ssb.sha256_bytes

from delta_dropbox_sav import (  # noqa: E402
    _DELTA_REF,
    apply_bytes_to_delta,
    augment_rom_sha1_map_from_delta_rom_blobs,
    build_delta_rows_by_rom_sha1,
    delta_modified_to_iso,
)
from game_id import (  # noqa: E402
    GameIdResolver,
    build_rom_sha1_to_game_id,
    remote_game_id_for_delta_title,
    sanitize_game_id,
    title_looks_like_retail_for_header,
)


def _sha1_file(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest().lower()


def _remote_cmp_ts(meta: dict[str, Any]) -> str:
    server_ts = str(meta.get("server_updated_at", "") or "")
    if server_ts:
        return server_ts
    return str(meta.get("last_modified_utc", ""))


def _iso_to_unix(s: str) -> float:
    if not s or not s.strip():
        return float("-inf")
    t = s.strip().replace("Z", "+00:00")
    try:
        dt = datetime.fromisoformat(t)
    except ValueError:
        return float("-inf")
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()


def _delta_meta_to_unix(meta_path: Path) -> float:
    j = json.loads(meta_path.read_text(encoding="utf-8"))
    sec = float((j.get("record") or {}).get("modifiedDate", 0))
    return (_DELTA_REF + timedelta(seconds=sec)).timestamp()


def _delta_gamesave_metadata_is_consistent(meta_path: Path, blob_path: Path) -> bool:
    """True when GameSave JSON hash fields and remoteIdentifier target match the blob bytes on disk."""
    try:
        j = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, UnicodeError):
        return False
    files = j.get("files")
    if not isinstance(files, list) or not files or not isinstance(files[0], dict):
        return False
    f0 = files[0]
    rec = j.get("record") or {}
    blob_sha1 = hashlib.sha1(blob_path.read_bytes()).hexdigest().lower()
    if str(f0.get("sha1Hash") or "").strip().lower() != blob_sha1:
        return False
    if str(rec.get("sha1") or "").strip().lower() != blob_sha1:
        return False
    ri = str(f0.get("remoteIdentifier") or "").strip().replace("\\", "/")
    if not ri:
        return False
    bn = ri.split("/")[-1].strip()
    if not bn:
        return False
    return bn == blob_path.name


def _header_gid_collisions(rom_to_gid: dict[str, str]) -> set[str]:
    """Header-derived ``game_id`` values shared by more than one ROM SHA-1 (common for hacks)."""
    inv: dict[str, list[str]] = {}
    for rsha, gid in rom_to_gid.items():
        inv.setdefault(gid, []).append(rsha)
    return {g for g, shas in inv.items() if len(shas) > 1}


def _log_if_server_header_not_mapped_to_delta(
    plan: dict[str, str | None],
    header_gid: str,
    hids: list[str],
    by_hid: dict[str, tuple[str, dict[str, Any]]],
    remote: dict[str, dict[str, Any]],
    log: Callable[[str], None],
) -> None:
    """When /save/{header_gid} exists but no Harmony row is mapped to it, 3DS data never reaches Dropbox."""
    if header_gid not in remote:
        return
    if any(plan.get(hid) == header_gid for hid in hids):
        return
    titles = [str(by_hid[h][1].get("name") or "?") for h in hids]
    log(
        f"[server_delta] note: server has /save/{header_gid} but **no Delta GameSave row** is mapped to "
        f"it this pass — there will be no `[dropbox] upload` for that save. "
        f"Delta titles in this header group: {titles}. "
        f"For retail Fire Red/Emerald from 3DS, open that **retail** game in Delta once so Harmony "
        f"creates a save slot; hacks need their own id on the server (e.g. PUT /save/redrocket)."
    )


def _dedupe_plan_server_targets(
    plan: dict[str, str | None],
    delta_by_rom: dict[str, dict[str, Any]],
    rom_to_gid: dict[str, str],
    remote: dict[str, dict[str, Any]],
    log: Callable[[str], None],
) -> None:
    """At most one Harmony row per server ``game_id`` — duplicate targets caused wrong saves on hacks."""
    hid_to_row: dict[str, dict[str, Any]] = {}
    hid_to_rs: dict[str, str] = {}
    for rs, row in delta_by_rom.items():
        hid = row["identifier"]
        if hid not in hid_to_row:
            hid_to_row[hid] = row
            hid_to_rs[hid] = rs

    by_gid: dict[str, list[str]] = {}
    for hid, gid in plan.items():
        if gid:
            by_gid.setdefault(gid, []).append(hid)

    for gid, hids in by_gid.items():
        if len(hids) <= 1:
            continue

        def rank(hid: str) -> tuple:
            row = hid_to_row[hid]
            name = str(row.get("name") or "")
            rs = hid_to_rs[hid]
            header = rom_to_gid.get(rs) or rom_to_gid.get(hid.strip().lower())
            matched = remote_game_id_for_delta_title(name, remote, header_hint=header)
            if matched == gid:
                return (0, name.lower(), hid)
            if header and gid == header and title_looks_like_retail_for_header(name, header):
                return (1, name.lower(), hid)
            if header and gid == header:
                return (2, name.lower(), hid)
            return (3, name.lower(), hid)

        hids_sorted = sorted(hids, key=rank)
        keeper = hids_sorted[0]
        for drop in hids_sorted[1:]:
            row = hid_to_row[drop]
            log(
                f"[skip] {row.get('name')!r}: server id {gid!r} already assigned to "
                f"{hid_to_row[keeper].get('name')!r} — duplicate mapping blocked"
            )
            plan[drop] = None


def _game_id_plan_for_server_delta(
    delta_by_rom: dict[str, dict[str, Any]],
    rom_to_gid: dict[str, str],
    remote: dict[str, dict[str, Any]],
    colliding: set[str],
    log: Callable[[str], None],
) -> dict[str, str | None]:
    """Map Harmony ``Game-{id}`` stem (``row['identifier']``) → server ``game_id`` or ``None`` (skip)."""
    plan: dict[str, str | None] = {}
    seen_hid: set[str] = set()
    rows_list: list[tuple[str, dict[str, Any], str]] = []
    for rom_sha, row in sorted(delta_by_rom.items(), key=lambda kv: kv[1].get("name") or ""):
        hid = row["identifier"]
        if hid in seen_hid:
            continue
        seen_hid.add(hid)
        canon_sha = hid.strip().lower()
        header_gid = rom_to_gid.get(canon_sha) or rom_to_gid.get(rom_sha)
        if not header_gid:
            continue
        rows_list.append((rom_sha, row, header_gid))

    groups: dict[str, list[tuple[str, dict[str, Any], str]]] = {}
    for rom_sha, row, header_gid in rows_list:
        if header_gid in colliding:
            groups.setdefault(header_gid, []).append((rom_sha, row, header_gid))

    for header_gid, items in groups.items():
        hids = [row["identifier"] for _, row, _ in items]
        by_hid = {row["identifier"]: (rs, row) for rs, row, _ in items}

        if len(items) == 1:
            rom_sha, row, _ = items[0]
            hid = row["identifier"]
            name = str(row.get("name") or "")
            name_key = remote_game_id_for_delta_title(name, remote, header_hint=header_gid)
            if name_key:
                plan[hid] = name_key
            elif header_gid in remote and title_looks_like_retail_for_header(name, header_gid):
                plan[hid] = header_gid
            elif header_gid in remote:
                plan[hid] = None
                log(
                    f"[skip] {name!r}: lone Delta title for header {header_gid!r} but name does not "
                    f"look like retail — refusing to apply that server save to this ROM "
                    f"(add PUT /save/{sanitize_game_id(name)})"
                )
            else:
                plan[hid] = None
            _log_if_server_header_not_mapped_to_delta(plan, header_gid, hids, by_hid, remote, log)
            continue

        explicit: dict[str, str] = {}
        for rom_sha, row, hg in items:
            hid = row["identifier"]
            name = str(row.get("name") or "")
            nk = remote_game_id_for_delta_title(name, remote, header_hint=header_gid)
            if nk and nk != header_gid:
                explicit[hid] = nk
            elif nk and nk == header_gid and title_looks_like_retail_for_header(name, header_gid):
                # Dedicated server row equals ROM header (e.g. pokemon-fire-bpre): map retail here so we
                # do not depend on sole-unmapped / first-retail ordering when /saves is briefly stale.
                explicit[hid] = nk

        # Safety: if a hack-specific server row has the same payload hash as the shared header row,
        # this is usually a mis-uploaded duplicate (observed to poison multiple titles and crash Delta).
        header_sha = str((remote.get(header_gid) or {}).get("sha256") or "").strip().lower()
        if header_sha:
            for hid, gid in list(explicit.items()):
                if gid == header_gid:
                    continue
                gid_sha = str((remote.get(gid) or {}).get("sha256") or "").strip().lower()
                if gid_sha and gid_sha == header_sha:
                    row = by_hid[hid][1]
                    plan[hid] = None
                    del explicit[hid]
                    log(
                        f"[skip] {row.get('name')!r}: /save/{gid} has same sha256 as /save/{header_gid}; "
                        f"refusing to apply duplicated payload across header-collision titles."
                    )

        for hid, gid in explicit.items():
            plan[hid] = gid

        unmapped_hids = [h for h in hids if h not in explicit]
        if not unmapped_hids:
            continue

        if len(unmapped_hids) == 1:
            hid = unmapped_hids[0]
            row = by_hid[hid][1]
            name = str(row.get("name") or "")
            if header_gid in remote and title_looks_like_retail_for_header(name, header_gid):
                plan[hid] = header_gid
            elif header_gid in remote:
                plan[hid] = None
                nk = sanitize_game_id(name)
                log(
                    f"[skip] {name!r}: last unmapped title for header {header_gid!r} is not retail-shaped "
                    f"— will not write {header_gid!r} save into this ROM (add PUT /save/{nk})"
                )
            else:
                plan[hid] = None
                log(
                    f"[skip] {name!r}: header {header_gid!r} not on server "
                    f"(collision group has no matching save)"
                )
            _log_if_server_header_not_mapped_to_delta(plan, header_gid, hids, by_hid, remote, log)
            continue

        retail_hid: str | None = None
        for hid in unmapped_hids:
            row = by_hid[hid][1]
            if title_looks_like_retail_for_header(str(row.get("name") or ""), header_gid):
                retail_hid = hid
                break

        if retail_hid and header_gid in remote:
            plan[retail_hid] = header_gid
            log(
                f"[server_delta] header {header_gid!r}: assigning shared server save to "
                f"{by_hid[retail_hid][1].get('name')!r} only (retail-shaped title)"
            )
            for hid in unmapped_hids:
                if hid == retail_hid:
                    continue
                row = by_hid[hid][1]
                nk = sanitize_game_id(str(row.get("name") or ""))
                log(
                    f"[skip] {row['name']!r}: shares GBA header with another Delta game — "
                    f"upload once under a unique server id (e.g. PUT /save/{nk}) to sync this hack"
                )
                plan[hid] = None
        else:
            stale = (
                f" (no /save/{header_gid} on server yet — retry after devices upload)"
                if header_gid not in remote
                else ""
            )
            for hid in unmapped_hids:
                row = by_hid[hid][1]
                nk = sanitize_game_id(str(row.get("name") or ""))
                log(
                    f"[skip] {row['name']!r}: ambiguous header {header_gid!r} "
                    f"({len(unmapped_hids)} titles){stale} — add PUT /save/<slug> per title or remove extras."
                )
                plan[hid] = None

        _log_if_server_header_not_mapped_to_delta(plan, header_gid, hids, by_hid, remote, log)

    for rom_sha, row, header_gid in rows_list:
        hid = row["identifier"]
        if hid in plan:
            continue
        name = str(row.get("name") or "")
        name_key = remote_game_id_for_delta_title(name, remote, header_hint=header_gid)
        if name_key:
            plan[hid] = name_key
        elif header_gid in remote:
            plan[hid] = header_gid
        else:
            plan[hid] = header_gid

    _dedupe_plan_server_targets(plan, delta_by_rom, rom_to_gid, remote, log)

    return plan


def _bootstrap_slot_map_ranked(
    *,
    slot_map: dict[str, str],
    delta_by_rom: dict[str, dict[str, Any]],
    rom_to_gid: dict[str, str],
    remote: dict[str, dict[str, Any]],
    gid_plan: dict[str, str | None],
    log: Callable[[str], None],
) -> bool:
    """Fill missing Harmony-slot mappings with deterministic ranking and persist later."""
    changed = False
    owner_by_gid: dict[str, str] = {}
    for hid, gid in slot_map.items():
        if gid in remote:
            owner_by_gid.setdefault(gid, hid)
    remote_by_rom_sha: dict[str, list[str]] = {}
    remote_by_sha256: dict[str, list[str]] = {}
    for gid, meta in remote.items():
        rsha = str(meta.get("rom_sha1") or "").strip().lower()
        if rsha:
            remote_by_rom_sha.setdefault(rsha, []).append(gid)
        dsha = str(meta.get("sha256") or "").strip().lower()
        if dsha:
            remote_by_sha256.setdefault(dsha, []).append(gid)

    seen_hid: set[str] = set()
    for rom_sha, row in sorted(delta_by_rom.items(), key=lambda kv: kv[1].get("name") or ""):
        hid = str(row["identifier"]).strip().lower()
        if hid in seen_hid:
            continue
        seen_hid.add(hid)
        if hid in slot_map and slot_map[hid] in remote:
            continue

        chosen: str | None = None
        reason = ""
        exact = remote_by_rom_sha.get(rom_sha.strip().lower(), [])
        if len(exact) == 1:
            chosen = exact[0]
            reason = "exact rom_sha1"
        if not chosen:
            header_gid = rom_to_gid.get(hid) or rom_to_gid.get(rom_sha.strip().lower())
            title_gid = remote_game_id_for_delta_title(str(row.get("name") or ""), remote, header_hint=header_gid)
            if title_gid:
                chosen = title_gid
                reason = "title/filename hint"
        if not chosen:
            blob_path = row.get("save_blob")
            if isinstance(blob_path, Path) and blob_path.is_file():
                dsha = sha256_bytes(blob_path.read_bytes())
                matches = remote_by_sha256.get(dsha, [])
                if len(matches) == 1:
                    chosen = matches[0]
                    reason = "unique payload hash"
        if not chosen:
            planned = gid_plan.get(hid)
            if planned and planned in remote:
                chosen = planned
                reason = "planner fallback"

        if not chosen:
            continue
        owner = owner_by_gid.get(chosen)
        if owner and owner != hid:
            continue
        slot_map[hid] = chosen
        owner_by_gid[chosen] = hid
        changed = True
        log(f"[slot-map] learned {row.get('name')!r}: {hid} -> {chosen} ({reason})")
    return changed


@dataclass
class TripleConfig:
    server_url: str
    api_key: str
    local_save_dir: Path
    delta_root: Path
    rom_dirs: list[Path]
    rom_map_path: Path | None
    delta_slot_map_path: Path | None
    rom_extensions: list[str]
    backup_dir: Path | None
    sync_mode: str  # "triple" | "server_delta"
    #: If True with ``server_delta``: when bytes differ, always **server → Delta** (never Delta → server).
    server_delta_one_way: bool = False
    #: In two-way mode, Delta must be at least this many seconds newer than server to win.
    server_delta_min_delta_win_seconds: int = 0
    #: In two-way mode, keep server as winner for this many seconds after a recent server update.
    server_delta_recent_server_protect_seconds: int = 0

    @classmethod
    def load(cls, path: Path) -> TripleConfig:
        raw = json.loads(path.read_text(encoding="utf-8"))
        rom_dirs = [Path(p).expanduser() for p in raw.get("rom_dirs", [])]
        rmp = Path(raw["rom_map_path"]).expanduser() if raw.get("rom_map_path") else None
        dsmp = Path(raw["delta_slot_map_path"]).expanduser() if raw.get("delta_slot_map_path") else None
        mode = str(raw.get("sync_mode", "triple")).strip().lower()
        if mode not in ("triple", "server_delta"):
            raise ValueError('sync_mode must be "triple" or "server_delta"')
        ow = raw.get("server_delta_one_way", False)
        if isinstance(ow, str):
            ow = ow.strip().lower() in ("1", "true", "yes")
        else:
            ow = bool(ow)
        min_win = int(raw.get("server_delta_min_delta_win_seconds", 0) or 0)
        if min_win < 0:
            min_win = 0
        recent_protect = int(raw.get("server_delta_recent_server_protect_seconds", 0) or 0)
        if recent_protect < 0:
            recent_protect = 0
        return cls(
            server_url=raw["server_url"].rstrip("/"),
            api_key=raw.get("api_key", ""),
            local_save_dir=Path(raw["local_save_dir"]).expanduser(),
            delta_root=Path(raw["delta_root"]).expanduser(),
            rom_dirs=rom_dirs,
            rom_map_path=rmp,
            delta_slot_map_path=dsmp,
            rom_extensions=raw.get("rom_extensions", [".gba"]),
            backup_dir=Path(raw["backup_dir"]).expanduser() if raw.get("backup_dir") else None,
            sync_mode=mode,
            server_delta_one_way=ow,
            server_delta_min_delta_win_seconds=min_win,
            server_delta_recent_server_protect_seconds=recent_protect,
        )

    def with_delta_root(self, path: Path) -> TripleConfig:
        """Same settings but a different on-disk Delta tree (e.g. API sync temp dir)."""
        return TripleConfig(
            server_url=self.server_url,
            api_key=self.api_key,
            local_save_dir=self.local_save_dir,
            delta_root=path,
            rom_dirs=self.rom_dirs,
            rom_map_path=self.rom_map_path,
            delta_slot_map_path=self.delta_slot_map_path,
            rom_extensions=self.rom_extensions,
            backup_dir=self.backup_dir,
            sync_mode=self.sync_mode,
            server_delta_one_way=self.server_delta_one_way,
            server_delta_min_delta_win_seconds=self.server_delta_min_delta_win_seconds,
            server_delta_recent_server_protect_seconds=self.server_delta_recent_server_protect_seconds,
        )


class TripleSync:
    def __init__(self, cfg: TripleConfig, dry_run: bool = False):
        self.cfg = cfg
        self.dry_run = dry_run
        self._session = requests.Session()
        self._session.headers.update({"X-API-Key": cfg.api_key})
        self._resolver = GameIdResolver(
            rom_dirs=cfg.rom_dirs,
            rom_map_path=cfg.rom_map_path,
            rom_extensions=cfg.rom_extensions,
        )

    def log(self, msg: str) -> None:
        print(msg, flush=True)

    def _load_delta_slot_map(self) -> dict[str, str]:
        p = self.cfg.delta_slot_map_path
        if not p:
            return {}
        try:
            if not p.is_file():
                return {}
            raw = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError, UnicodeError):
            return {}
        if not isinstance(raw, dict):
            return {}
        out: dict[str, str] = {}
        for k, v in raw.items():
            hk = str(k).strip().lower()
            gv = str(v).strip().lower()
            if hk and gv:
                out[hk] = gv
        return out

    def _save_delta_slot_map(self, slot_map: dict[str, str]) -> None:
        p = self.cfg.delta_slot_map_path
        if not p or self.dry_run:
            return
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(p.suffix + ".tmp")
        tmp.write_text(json.dumps(slot_map, indent=2, sort_keys=True), encoding="utf-8")
        os.replace(tmp, p)

    def _list_remote(self) -> dict[str, dict[str, Any]]:
        resp = self._session.get(f"{self.cfg.server_url}/saves", timeout=20)
        resp.raise_for_status()
        saves = resp.json().get("saves", [])
        return {item["game_id"]: item for item in saves}

    def _upload_server(
        self,
        game_id: str,
        data: bytes,
        filename: str,
        last_modified_utc: str,
        *,
        rom_sha1: str | None = None,
    ) -> None:
        params = {
            "last_modified_utc": last_modified_utc,
            "sha256": sha256_bytes(data),
            "size_bytes": len(data),
            "filename_hint": filename,
            "platform_source": "delta-folder-sync",
            "force": "1",
        }
        if rom_sha1:
            params["rom_sha1"] = rom_sha1
        if self.dry_run:
            self.log(f"[dry-run] PUT server {game_id} ({len(data)} B)")
            return
        r = self._session.put(
            f"{self.cfg.server_url}/save/{game_id}",
            params=params,
            data=data,
            timeout=60,
        )
        r.raise_for_status()
        info = r.json()
        if not info.get("applied", True):
            self.log(f"[skip-server] {game_id}: server kept existing (no-op)")
        else:
            self.log(f"[upload] {game_id} <- {filename}")

    def _download_server(self, game_id: str) -> bytes:
        if self.dry_run:
            self.log(f"[dry-run] GET server {game_id}")
            return b""
        r = self._session.get(f"{self.cfg.server_url}/save/{game_id}", timeout=60)
        r.raise_for_status()
        return r.content

    def _write_local_atomic(self, path: Path, data: bytes) -> None:
        if self.dry_run:
            self.log(f"[dry-run] write local {path.name} ({len(data)} B)")
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_bytes(data)
        os.replace(tmp, path)

    @staticmethod
    def _mirror_sav_path(local_save_dir: Path, game_id: str, srv_meta: dict[str, Any] | None) -> Path:
        hint = str((srv_meta or {}).get("filename_hint") or "").strip()
        if hint:
            name = Path(hint).name
            if not name.lower().endswith(".sav"):
                name = f"{game_id}.sav"
        else:
            name = f"{game_id}.sav"
        return local_save_dir / name

    def _sync_server_delta_once(self) -> None:
        """Merge server (devices) ↔ Delta only; mirror winner to ``local_save_dir``."""
        remote = self._list_remote()
        delta_by_rom = build_delta_rows_by_rom_sha1(self.cfg.delta_root)
        hid_rows: dict[str, dict[str, Any]] = {}
        for _rs, row in delta_by_rom.items():
            hid_rows.setdefault(str(row["identifier"]).strip().lower(), row)
        rom_to_gid = build_rom_sha1_to_game_id(
            self.cfg.rom_dirs,
            self.cfg.rom_extensions,
            self.cfg.rom_map_path,
        )
        n_from_blobs = augment_rom_sha1_map_from_delta_rom_blobs(rom_to_gid, self.cfg.delta_root)
        self.log(
            f"[server_delta] delta games with save: {len(delta_by_rom)}, "
            f"ROM SHA-1 map entries: {len(rom_to_gid)}"
            + (f" ({n_from_blobs} from Delta ROM blobs)" if n_from_blobs else "")
        )

        colliding = _header_gid_collisions(rom_to_gid)
        if colliding:
            self.log(
                f"[server_delta] GBA header game_id shared by multiple ROMs (hacks?): "
                f"{', '.join(sorted(colliding))} — using server keys from Delta titles when present"
            )

        gid_plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, self.log)
        slot_map = self._load_delta_slot_map()
        if _bootstrap_slot_map_ranked(
            slot_map=slot_map,
            delta_by_rom=delta_by_rom,
            rom_to_gid=rom_to_gid,
            remote=remote,
            gid_plan=gid_plan,
            log=self.log,
        ):
            self._save_delta_slot_map(slot_map)
        if slot_map:
            for hid, gid in slot_map.items():
                row = hid_rows.get(hid)
                if not row:
                    continue
                if gid in remote:
                    prev = gid_plan.get(hid)
                    if prev and prev != gid:
                        self.log(
                            f"[slot-map] override {row.get('name')!r}: {prev!r} -> {gid!r} "
                            f"(from {self.cfg.delta_slot_map_path})"
                        )
                    gid_plan[hid] = gid
                else:
                    self.log(
                        f"[slot-map] stale mapping for {row.get('name')!r}: /save/{gid} not on server"
                    )

        seen_harmony_id: set[str] = set()
        slot_map_changed = False
        for rom_sha, row in sorted(delta_by_rom.items(), key=lambda kv: kv[1].get("name") or ""):
            hid = row["identifier"]
            if hid in seen_harmony_id:
                continue
            seen_harmony_id.add(hid)

            canon_sha = hid.strip().lower()
            header_gid = rom_to_gid.get(canon_sha) or rom_to_gid.get(rom_sha)
            if not header_gid:
                self.log(f"[skip-delta] no ROM match for {row['name']} (sha1 {rom_sha[:8]}…)")
                continue

            game_id = gid_plan.get(hid)
            if not game_id:
                self.log(
                    f"[skip-map] {row['name']!r}: no server save mapped to this Delta slot "
                    f"(collision rules, dedupe, or missing /save/<id> on server — see prior [skip] lines)"
                )
                continue

            did = row["identifier"]
            meta_path = row["save_json"]
            blob_path = row["save_blob"]
            assert meta_path and blob_path

            srv_meta = remote.get(game_id)
            t_del = _delta_meta_to_unix(meta_path)
            delta_bytes = blob_path.read_bytes()
            sd = sha256_bytes(delta_bytes)

            if srv_meta is None:
                self.log(f"[skip] {game_id} ({row['name']}): not on server — upload from Delta omitted")
                continue
            hid_l = str(hid).strip().lower()
            if hid_l not in slot_map:
                owner = next((h for h, g in slot_map.items() if g == game_id and h != hid_l), None)
                if owner is None:
                    slot_map[hid_l] = game_id
                    slot_map_changed = True
                    self.log(f"[slot-map] learned {row['name']!r}: {hid_l} -> {game_id}")
                else:
                    self.log(
                        f"[slot-map] not learning {row['name']!r} -> {game_id}: already owned by slot {owner}"
                    )

            t_srv = _iso_to_unix(_remote_cmp_ts(srv_meta))
            ss = str(srv_meta.get("sha256", "") or "").lower()

            if ss and sd == ss:
                if _delta_gamesave_metadata_is_consistent(meta_path, blob_path):
                    continue
                self.log(
                    f"[server_delta] {game_id} ({row['name']}) metadata drift detected; "
                    f"repairing GameSave JSON to match blob bytes"
                )
                if not self.dry_run:
                    apply_bytes_to_delta(self.cfg.delta_root, did, delta_bytes, self.cfg.backup_dir)
                continue

            if self.cfg.sync_mode == "server_delta" and self.cfg.server_delta_one_way:
                winner_name = "server"
                self.log(
                    f"[server_delta] {game_id} ({row['name']}) one_way: server → Delta "
                    f"(server={t_srv:.0f} delta={t_del:.0f}, hashes differ)"
                )
            else:
                candidates_ts = [("server", t_srv), ("delta", t_del)]
                winner_name, winner_ts = max(candidates_ts, key=lambda c: c[1])
                if (
                    winner_name == "delta"
                    and t_srv > float("-inf")
                    and self.cfg.server_delta_recent_server_protect_seconds > 0
                ):
                    now_ts = datetime.now(timezone.utc).timestamp()
                    age = now_ts - t_srv
                    if age < float(self.cfg.server_delta_recent_server_protect_seconds):
                        self.log(
                            f"[server_delta] {game_id} ({row['name']}) recent server update age={age:.0f}s "
                            f"< protect {self.cfg.server_delta_recent_server_protect_seconds}s; keeping server winner"
                        )
                        winner_name = "server"
                        winner_ts = t_srv
                if (
                    winner_name == "delta"
                    and t_srv > float("-inf")
                    and self.cfg.server_delta_min_delta_win_seconds > 0
                ):
                    lead = t_del - t_srv
                    if lead < float(self.cfg.server_delta_min_delta_win_seconds):
                        self.log(
                            f"[server_delta] {game_id} ({row['name']}) delta lead {lead:.0f}s "
                            f"< min {self.cfg.server_delta_min_delta_win_seconds}s; keeping server winner"
                        )
                        winner_name = "server"
                        winner_ts = t_srv
                if winner_ts == float("-inf"):
                    self.log(f"[skip] {game_id}: no usable timestamps")
                    continue
                self.log(
                    f"[server_delta] {game_id} ({row['name']}) winner={winner_name} "
                    f"(server={t_srv:.0f} delta={t_del:.0f})"
                )
                if winner_name == "delta" and ss and sd != ss:
                    self.log(
                        f"[server_delta] hint: Harmony modifiedDate won — server bytes are NOT written into "
                        f"the Dropbox folder. Set GBASYNC_SERVER_DELTA_ONE_WAY=true in .env so 3DS/server "
                        f"wins whenever hashes differ."
                    )

            mirror = self._mirror_sav_path(self.cfg.local_save_dir, game_id, srv_meta)

            if winner_name == "server":
                data = self._download_server(game_id)
                self._write_local_atomic(mirror, data)
                if not self.dry_run:
                    apply_bytes_to_delta(self.cfg.delta_root, did, data, self.cfg.backup_dir)
            else:
                ts_iso = delta_modified_to_iso(meta_path)
                self._upload_server(game_id, delta_bytes, mirror.name, ts_iso, rom_sha1=rom_sha)
                self._write_local_atomic(mirror, delta_bytes)
                if not self.dry_run and not _delta_gamesave_metadata_is_consistent(meta_path, blob_path):
                    self.log(
                        f"[server_delta] {game_id} ({row['name']}) repairing GameSave JSON in same pass "
                        f"after delta-winner upload"
                    )
                    apply_bytes_to_delta(self.cfg.delta_root, did, delta_bytes, self.cfg.backup_dir)
        if slot_map_changed:
            self._save_delta_slot_map(slot_map)

    def _sync_server_only_for_paths(self, paths: list[Path], remote: dict[str, dict[str, Any]]) -> None:
        """Mirror ``bridge.py`` for a subset of files (no Delta)."""
        by_gid: dict[str, list[Path]] = {}
        for p in paths:
            by_gid.setdefault(self._resolver.resolve(p), []).append(p)
        local_by_id: dict[str, Path] = {}
        for gid, plist in by_gid.items():
            if len(plist) > 1:
                plist = sorted(plist, key=lambda x: x.stat().st_mtime, reverse=True)
                self.log(
                    f"[dual] {len(plist)} local .sav files resolve to server id {gid!r}; "
                    f"using newest mtime: {plist[0].name}"
                )
            local_by_id[gid] = plist[0]
        for game_id, path in local_by_id.items():
            local_mtime_iso = utc_iso_for_local_sav(path)
            local_sha = sha256_bytes(path.read_bytes())
            meta = remote.get(game_id)
            if meta is None:
                rom_sha1: str | None = None
                rom = self._resolver.resolve_rom_path(path)
                if rom and rom.is_file():
                    rom_sha1 = _sha1_file(rom)
                self._upload_server(game_id, path.read_bytes(), path.name, local_mtime_iso, rom_sha1=rom_sha1)
                continue
            remote_sha = str(meta.get("sha256", "") or "")
            if remote_sha and local_sha == remote_sha:
                continue
            remote_ts = _remote_cmp_ts(meta)
            if local_mtime_iso > remote_ts:
                rom_sha1 = None
                rom = self._resolver.resolve_rom_path(path)
                if rom and rom.is_file():
                    rom_sha1 = _sha1_file(rom)
                self._upload_server(game_id, path.read_bytes(), path.name, local_mtime_iso, rom_sha1=rom_sha1)

        remote = self._list_remote()
        for game_id, meta in remote.items():
            path = local_by_id.get(game_id)
            if path is None:
                continue
            local_mtime_iso = utc_iso_for_local_sav(path)
            remote_ts = _remote_cmp_ts(meta)
            if remote_ts > local_mtime_iso:
                data = self._download_server(game_id)
                if not self.dry_run:
                    self._write_local_atomic(path, data)

    def sync_once(self) -> None:
        self.cfg.local_save_dir.mkdir(parents=True, exist_ok=True)
        if self.cfg.sync_mode == "server_delta":
            self._sync_server_delta_once()
            return

        remote = self._list_remote()
        delta_by_rom = build_delta_rows_by_rom_sha1(self.cfg.delta_root)

        local_files = sorted(self.cfg.local_save_dir.glob("*.sav"))
        linked_paths: list[Path] = []
        rom_sha_for_path: dict[Path, str] = {}

        for path in local_files:
            rom = self._resolver.resolve_rom_path(path)
            if not rom or not rom.is_file():
                continue
            rs = _sha1_file(rom)
            if rs in delta_by_rom:
                linked_paths.append(path)
                rom_sha_for_path[path] = rs

        dual_paths = [p for p in local_files if p not in linked_paths]
        if dual_paths:
            self.log(f"[dual] server ↔ local only: {len(dual_paths)} file(s)")
            self._sync_server_only_for_paths(dual_paths, remote)

        by_rs: dict[str, list[Path]] = {}
        for path in linked_paths:
            by_rs.setdefault(rom_sha_for_path[path], []).append(path)
        for rs, paths in by_rs.items():
            if len(paths) > 1:
                paths = sorted(paths, key=lambda p: p.stat().st_mtime, reverse=True)
                self.log(
                    f"[triple] {len(paths)} local .sav files link to the same Delta ROM; "
                    f"using newest mtime: {paths[0].name}"
                )
            path = paths[0]
            row = delta_by_rom[rs]
            did = row["identifier"]
            meta_path = row["save_json"]
            blob_path = row["save_blob"]
            assert meta_path and blob_path

            game_id = self._resolver.resolve(path)

            local_bytes = path.read_bytes()
            delta_bytes = blob_path.read_bytes()
            srv_meta = remote.get(game_id)

            t_loc = path.stat().st_mtime
            t_srv = _iso_to_unix(_remote_cmp_ts(srv_meta)) if srv_meta else float("-inf")
            t_del = _delta_meta_to_unix(meta_path)

            if srv_meta is None:
                candidates_ts = [("local", t_loc), ("delta", t_del)]
            else:
                candidates_ts = [("local", t_loc), ("server", t_srv), ("delta", t_del)]

            sl = sha256_bytes(local_bytes)
            ss = str(srv_meta.get("sha256", "") or "").lower() if srv_meta else ""
            sd = sha256_bytes(delta_bytes)

            if ss and sl == ss == sd:
                self.log(f"[ok] {game_id}: all three match")
                continue

            winner_name, winner_ts = max(candidates_ts, key=lambda c: c[1])
            if winner_ts == float("-inf"):
                self.log(f"[skip] {game_id}: no usable timestamps")
                continue

            self.log(
                f"[triple] {game_id} ({row['name']}) winner={winner_name} "
                f"(local={t_loc:.0f} server={t_srv:.0f} delta={t_del:.0f})"
            )

            def _touch_local(ts: float) -> None:
                if not self.dry_run and path.exists():
                    os.utime(path, (ts, ts))

            if winner_name == "local":
                lm = utc_iso_for_local_sav(path)
                self._upload_server(game_id, local_bytes, path.name, lm, rom_sha1=rs)
                if not self.dry_run:
                    apply_bytes_to_delta(self.cfg.delta_root, did, local_bytes, self.cfg.backup_dir)

            elif winner_name == "server":
                data = self._download_server(game_id)
                self._write_local_atomic(path, data)
                if not self.dry_run:
                    _touch_local(t_srv if t_srv > float("-inf") else t_loc)
                    apply_bytes_to_delta(self.cfg.delta_root, did, data, self.cfg.backup_dir)

            else:
                self._write_local_atomic(path, delta_bytes)
                if not self.dry_run:
                    _touch_local(t_del)
                ts_iso = datetime.fromtimestamp(t_del, tz=timezone.utc).replace(microsecond=0).isoformat()
                self._upload_server(game_id, delta_bytes, path.name, ts_iso, rom_sha1=rs)


def main() -> None:
    p = argparse.ArgumentParser(description="GBAsync server ↔ Delta Emulator Dropbox folder (Harmony)")
    p.add_argument("--config", type=Path, required=True)
    p.add_argument("--once", action="store_true", help="single pass (default)")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if not args.once:
        raise SystemExit("Only --once is implemented (run from cron or a loop externally).")

    cfg = TripleConfig.load(args.config.expanduser().resolve())
    cfg.local_save_dir.mkdir(parents=True, exist_ok=True)
    if not cfg.delta_root.is_dir():
        raise SystemExit(f"Not a directory: {cfg.delta_root}")

    TripleSync(cfg, dry_run=args.dry_run).sync_once()


if __name__ == "__main__":
    main()
