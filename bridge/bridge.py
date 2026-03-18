#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import requests
from watchdog.events import FileSystemEvent, FileSystemEventHandler
from watchdog.observers import Observer

from game_id import GameIdResolver


def utc_iso_from_mtime(mtime: float) -> str:
    return datetime.fromtimestamp(mtime, timezone.utc).replace(microsecond=0).isoformat()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass
class Config:
    server_url: str
    api_key: str
    delta_save_dir: Path
    poll_seconds: int
    rom_dirs: list[Path]
    rom_map_path: Path | None
    rom_extensions: list[str]

    @classmethod
    def load(cls, path: Path) -> "Config":
        raw = json.loads(path.read_text(encoding="utf-8"))
        rom_dirs = [Path(p).expanduser() for p in raw.get("rom_dirs", [])]
        rom_map_path = Path(raw["rom_map_path"]).expanduser() if raw.get("rom_map_path") else None
        return cls(
            server_url=raw["server_url"].rstrip("/"),
            api_key=raw.get("api_key", ""),
            delta_save_dir=Path(raw["delta_save_dir"]).expanduser(),
            poll_seconds=int(raw.get("poll_seconds", 30)),
            rom_dirs=rom_dirs,
            rom_map_path=rom_map_path,
            rom_extensions=raw.get("rom_extensions", [".gba"]),
        )


class SyncBridge:
    def __init__(self, config: Config, dry_run: bool = False, verbose: bool = False):
        self.cfg = config
        self.dry_run = dry_run
        self.verbose = verbose
        self._session = requests.Session()
        self._session.headers.update({"X-API-Key": self.cfg.api_key})
        self._lock = threading.Lock()
        self._game_id_resolver = GameIdResolver(
            rom_dirs=self.cfg.rom_dirs,
            rom_map_path=self.cfg.rom_map_path,
            rom_extensions=self.cfg.rom_extensions,
        )

    def log(self, msg: str) -> None:
        print(msg, flush=True)

    def debug(self, msg: str) -> None:
        if self.verbose:
            self.log(f"[debug] {msg}")

    def _list_remote(self) -> dict[str, dict[str, Any]]:
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

    def _upload_file(self, path: Path) -> None:
        if path.suffix.lower() != ".sav":
            return
        data = path.read_bytes()
        game_id = self._game_id_resolver.resolve(path)
        mtime = path.stat().st_mtime
        last_modified_utc = utc_iso_from_mtime(mtime)
        digest = sha256_bytes(data)
        params = {
            "last_modified_utc": last_modified_utc,
            "sha256": digest,
            "size_bytes": len(data),
            "filename_hint": path.name,
            "platform_source": "delta-bridge",
        }
        if self.dry_run:
            self.log(f"[dry-run] upload {path.name} -> {game_id}")
            return
        resp = self._session.put(f"{self.cfg.server_url}/save/{game_id}", params=params, data=data, timeout=30)
        resp.raise_for_status()
        info = resp.json()
        if info.get("conflict"):
            self.log(f"[conflict] server marked conflict for {path.name}")
        else:
            self.log(f"[upload] {path.name} -> {game_id}")

    def _download_to_path(self, game_id: str, filename_hint: str | None) -> None:
        target_name = filename_hint if filename_hint and filename_hint.endswith(".sav") else f"{game_id}.sav"
        target = self.cfg.delta_save_dir / target_name
        if self.dry_run:
            self.log(f"[dry-run] download {game_id} -> {target.name}")
            return
        resp = self._session.get(f"{self.cfg.server_url}/save/{game_id}", timeout=30)
        resp.raise_for_status()
        tmp = target.with_suffix(".tmp")
        tmp.write_bytes(resp.content)
        os.replace(tmp, target)
        self.log(f"[download] {game_id} -> {target.name}")

    def sync_once(self) -> None:
        with self._lock:
            self.cfg.delta_save_dir.mkdir(parents=True, exist_ok=True)
            remote = self._list_remote()
            local_files = sorted(self.cfg.delta_save_dir.glob("*.sav"))
            local_by_id = {self._game_id_resolver.resolve(p): p for p in local_files}

            # upload local-newer or local-only
            for game_id, path in local_by_id.items():
                local_mtime_iso = utc_iso_from_mtime(path.stat().st_mtime)
                local_sha = sha256_bytes(path.read_bytes())
                remote_meta = remote.get(game_id)
                if remote_meta is None:
                    self._upload_file(path)
                    continue
                remote_sha = str(remote_meta.get("sha256", "") or "")
                if remote_sha and local_sha == remote_sha:
                    continue
                if remote_sha:
                    self._upload_file(path)
                    continue
                # Fallback path for old metadata records without sha256.
                remote_ts = str(remote_meta.get("last_modified_utc", "") or self._remote_cmp_ts(remote_meta))
                if local_mtime_iso > remote_ts:
                    self._upload_file(path)

            # download remote-only or remote-newer
            remote = self._list_remote()
            for game_id, meta in remote.items():
                local = local_by_id.get(game_id)
                if local is None:
                    self._download_to_path(game_id, meta.get("filename_hint"))
                    continue
                local_mtime_iso = utc_iso_from_mtime(local.stat().st_mtime)
                remote_ts = self._remote_cmp_ts(meta)
                if remote_ts > local_mtime_iso:
                    self._download_to_path(game_id, meta.get("filename_hint"))


class SaveEventHandler(FileSystemEventHandler):
    def __init__(self, bridge: SyncBridge):
        self.bridge = bridge

    def on_modified(self, event: FileSystemEvent) -> None:
        if event.is_directory:
            return
        path = Path(event.src_path)
        if path.suffix.lower() == ".sav":
            try:
                self.bridge._upload_file(path)
            except Exception as exc:  # noqa: BLE001
                self.bridge.log(f"[error] watch upload failed for {path.name}: {exc}")

    def on_created(self, event: FileSystemEvent) -> None:
        self.on_modified(event)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Delta <-> SaveSync bridge")
    parser.add_argument("--config", required=True, help="Path to bridge JSON config")
    parser.add_argument("--watch", action="store_true", help="Run continuous sync loop")
    parser.add_argument("--once", action="store_true", help="Run one sync pass then exit")
    parser.add_argument("--dry-run", action="store_true", help="Show actions only")
    parser.add_argument("--verbose", action="store_true", help="Enable debug logs")
    return parser.parse_args()


def run_watch(bridge: SyncBridge) -> None:
    event_handler = SaveEventHandler(bridge)
    observer = Observer()
    observer.schedule(event_handler, str(bridge.cfg.delta_save_dir), recursive=False)
    observer.start()
    bridge.log("[watch] started")
    try:
        while True:
            try:
                bridge.sync_once()
            except Exception as exc:  # noqa: BLE001
                bridge.log(f"[error] periodic sync failed: {exc}")
            time.sleep(bridge.cfg.poll_seconds)
    except KeyboardInterrupt:
        bridge.log("[watch] stopping")
    finally:
        observer.stop()
        observer.join()


def main() -> None:
    args = parse_args()
    if not args.watch and not args.once:
        raise SystemExit("Choose --once or --watch")
    cfg = Config.load(Path(args.config))
    bridge = SyncBridge(cfg, dry_run=args.dry_run, verbose=args.verbose)
    if args.once:
        bridge.sync_once()
        return
    run_watch(bridge)


if __name__ == "__main__":
    main()
