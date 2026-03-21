#!/usr/bin/env python3
"""
Sync a Dropbox folder of plain ``*.sav`` files with the GBAsync server.

This is **not** the same as Delta’s built-in Dropbox sync. Delta uses the Harmony
framework and stores opaque bundles in cloud storage — not a flat folder of
``.sav`` files the bridge can read.

Use this when you keep (or copy) real ``.sav`` files in a Dropbox path you
control, e.g. ``/GBAsync/gba`` in app-folder scope, and want them to stay in
sync with your self-hosted GBAsync server alongside Switch/3DS.

Credentials: repository-root ``.env`` or shell exports — see ``DROPBOX_SETUP.md``.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import requests

try:
    import dropbox
    from dropbox.exceptions import ApiError
    from dropbox.files import FileMetadata
except ImportError as exc:  # pragma: no cover - import guard
    raise SystemExit("Install the Dropbox SDK: pip install -r requirements-dropbox.txt") from exc

from dropbox_env import make_dropbox_client
from game_id import GameIdResolver


def _utc_iso_from_dt(dt: datetime) -> str:
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc).replace(microsecond=0).isoformat()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass
class DropboxBridgeConfig:
    server_url: str
    api_key: str
    remote_folder: str
    poll_seconds: int
    rom_dirs: list[Path]
    rom_map_path: Path | None
    rom_extensions: list[str]

    @classmethod
    def load(cls, path: Path) -> DropboxBridgeConfig:
        raw = json.loads(path.read_text(encoding="utf-8"))
        dbx = raw.get("dropbox") or {}
        folder = str(dbx.get("remote_folder", "")).strip()
        if not folder:
            raise ValueError('config missing dropbox.remote_folder (e.g. "/GBAsync/gba")')
        if not folder.startswith("/"):
            folder = "/" + folder
        rom_dirs = [Path(p).expanduser() for p in raw.get("rom_dirs", [])]
        rom_map_path = Path(raw["rom_map_path"]).expanduser() if raw.get("rom_map_path") else None
        return cls(
            server_url=raw["server_url"].rstrip("/"),
            api_key=raw.get("api_key", ""),
            remote_folder=folder.rstrip("/"),
            poll_seconds=int(raw.get("poll_seconds", 60)),
            rom_dirs=rom_dirs,
            rom_map_path=rom_map_path,
            rom_extensions=raw.get("rom_extensions", [".gba"]),
        )


class DropboxServerBridge:
    def __init__(self, cfg: DropboxBridgeConfig, dbx: dropbox.Dropbox, dry_run: bool = False):
        self.cfg = cfg
        self.dbx = dbx
        self.dry_run = dry_run
        self._session = requests.Session()
        self._session.headers.update({"X-API-Key": self.cfg.api_key})
        self._resolver = GameIdResolver(
            rom_dirs=self.cfg.rom_dirs,
            rom_map_path=self.cfg.rom_map_path,
            rom_extensions=self.cfg.rom_extensions,
        )

    def log(self, msg: str) -> None:
        print(msg, flush=True)

    def _list_remote_server(self) -> dict[str, dict[str, Any]]:
        resp = self._session.get(f"{self.cfg.server_url}/saves", timeout=20)
        resp.raise_for_status()
        saves = resp.json().get("saves", [])
        return {item["game_id"]: item for item in saves}

    @staticmethod
    def _remote_cmp_ts(meta: dict[str, Any]) -> str:
        server_ts = str(meta.get("server_updated_at", "") or "")
        if server_ts:
            return server_ts
        return str(meta.get("last_modified_utc", ""))

    def _list_dropbox_savs(self) -> dict[str, tuple[str, datetime, bytes]]:
        """Map ``game_id`` -> (dropbox_path_lower, server_modified, data)."""
        out: dict[str, tuple[str, datetime, bytes]] = {}
        result = self.dbx.files_list_folder(self.cfg.remote_folder)
        while True:
            for ent in result.entries:
                if not isinstance(ent, FileMetadata):
                    continue
                if not ent.name.lower().endswith(".sav"):
                    path = ent.path_display
                    _md, res = self.dbx.files_download(path)
                    data = res.content
                    stem = Path(ent.name).stem
                    gid = self._resolver.resolve_stem(stem)
                    modified = ent.server_modified
                    out[gid] = (path, modified, data)
            if not result.has_more:
                break
            result = self.dbx.files_list_folder_continue(result.cursor)
        return out

    def _upload_server(self, game_id: str, data: bytes, filename: str, last_modified_utc: str) -> None:
        digest = sha256_bytes(data)
        params = {
            "last_modified_utc": last_modified_utc,
            "sha256": digest,
            "size_bytes": len(data),
            "filename_hint": filename,
            "platform_source": "dropbox-bridge",
            "force": "1",
        }
        if self.dry_run:
            self.log(f"[dry-run] PUT server {filename} -> {game_id}")
            return
        resp = self._session.put(
            f"{self.cfg.server_url}/save/{game_id}",
            params=params,
            data=data,
            timeout=60,
        )
        resp.raise_for_status()
        info = resp.json()
        if not info.get("applied", True):
            self.log(f"[rejected] server kept existing for {game_id}")
        elif info.get("conflict"):
            self.log(f"[conflict] {game_id}")
        else:
            self.log(f"[upload->server] {filename} -> {game_id}")

    def _download_server_to_dropbox(self, game_id: str, filename_hint: str | None) -> None:
        name = filename_hint if filename_hint and filename_hint.lower().endswith(".sav") else f"{game_id}.sav"
        drop_path = f"{self.cfg.remote_folder}/{name}"
        if self.dry_run:
            self.log(f"[dry-run] GET server {game_id} -> {drop_path}")
            return
        resp = self._session.get(f"{self.cfg.server_url}/save/{game_id}", timeout=60)
        resp.raise_for_status()
        data = resp.content
        self.dbx.files_upload(data, drop_path, mode=dropbox.files.WriteMode.overwrite)
        self.log(f"[download->dropbox] {game_id} -> {name}")

    def sync_once(self) -> None:
        remote_srv = self._list_remote_server()
        try:
            drop_map = self._list_dropbox_savs()
        except ApiError as exc:
            self.log(f"[error] Dropbox list/download failed: {exc}")
            raise

        # game_id -> (path, mtime, bytes)
        for game_id, (_path, modified, data) in drop_map.items():
            local_ts = _utc_iso_from_dt(modified)
            local_sha = sha256_bytes(data)
            meta = remote_srv.get(game_id)
            filename = Path(_path).name
            if meta is None:
                self._upload_server(game_id, data, filename, local_ts)
                continue
            remote_sha = str(meta.get("sha256", "") or "")
            if remote_sha and local_sha == remote_sha:
                continue
            remote_ts = self._remote_cmp_ts(meta)
            if local_ts > remote_ts:
                self._upload_server(game_id, data, filename, local_ts)

        remote_srv = self._list_remote_server()
        drop_map = self._list_dropbox_savs()
        drop_by_id = {gid: (p, m, d) for gid, (p, m, d) in drop_map.items()}

        for game_id, meta in remote_srv.items():
            entry = drop_by_id.get(game_id)
            if entry is None:
                self._download_server_to_dropbox(game_id, meta.get("filename_hint"))
                continue
            _path, modified, _data = entry
            local_ts = _utc_iso_from_dt(modified)
            remote_ts = self._remote_cmp_ts(meta)
            if remote_ts > local_ts:
                self._download_server_to_dropbox(game_id, meta.get("filename_hint"))


def main() -> None:
    parser = argparse.ArgumentParser(description="Dropbox .sav folder <-> GBAsync server")
    parser.add_argument("--config", required=True, help="JSON config (see config.example.dropbox.json)")
    parser.add_argument("--once", action="store_true", help="Single sync pass")
    parser.add_argument("--watch", action="store_true", help="Loop every poll_seconds")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if not args.once and not args.watch:
        raise SystemExit("Choose --once or --watch")

    cfg = DropboxBridgeConfig.load(Path(args.config))
    dbx = make_dropbox_client()
    bridge = DropboxServerBridge(cfg, dbx, dry_run=args.dry_run)

    from dropbox_run_lock import dropbox_run_lock

    if args.once:
        with dropbox_run_lock():
            bridge.sync_once()
        return

    bridge.log("[watch] Dropbox <-> GBAsync (Ctrl+C to stop)")
    while True:
        try:
            with dropbox_run_lock():
                bridge.sync_once()
        except Exception as exc:  # noqa: BLE001
            bridge.log(f"[error] sync failed: {exc}")
        time.sleep(cfg.poll_seconds)


if __name__ == "__main__":
    # Ensure `python path/to/dropbox_bridge.py` finds `game_id.py` next to this file.
    _here = str(Path(__file__).resolve().parent)
    if _here not in sys.path:
        sys.path.insert(0, _here)
    main()
