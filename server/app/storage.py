from __future__ import annotations

import hashlib
import json
import os
import shutil
from datetime import datetime, timezone
from pathlib import Path
from threading import Lock

from .models import SaveMeta, SaveListItem


def parse_utc(iso_value: str) -> datetime:
    return datetime.fromisoformat(iso_value.replace("Z", "+00:00")).astimezone(timezone.utc)


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


class SaveStore:
    def __init__(self, save_root: Path, history_root: Path, index_path: Path, keep_history: bool = True):
        self.save_root = save_root
        self.history_root = history_root
        self.index_path = index_path
        self.keep_history = keep_history
        self._lock = Lock()
        self.save_root.mkdir(parents=True, exist_ok=True)
        self.history_root.mkdir(parents=True, exist_ok=True)
        self.index_path.parent.mkdir(parents=True, exist_ok=True)
        if not self.index_path.exists():
            self._write_index({})

    def _read_index(self) -> dict[str, dict]:
        with self.index_path.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) else {}

    def _write_index(self, data: dict[str, dict]) -> None:
        tmp = self.index_path.with_suffix(".tmp")
        with tmp.open("w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2, sort_keys=True)
        os.replace(tmp, self.index_path)

    def save_path(self, game_id: str) -> Path:
        return self.save_root / f"{game_id}.sav"

    def history_dir(self, game_id: str) -> Path:
        path = self.history_root / game_id
        path.mkdir(parents=True, exist_ok=True)
        return path

    def list_saves(self) -> list[SaveListItem]:
        with self._lock:
            index = self._read_index()
        out: list[SaveListItem] = []
        for game_id, raw in index.items():
            out.append(SaveListItem(game_id=game_id, **raw))
        out.sort(key=lambda item: item.game_id)
        return out

    def list_conflicts(self) -> list[SaveListItem]:
        return [item for item in self.list_saves() if item.conflict]

    def get_meta(self, game_id: str) -> SaveMeta | None:
        with self._lock:
            index = self._read_index()
        raw = index.get(game_id)
        if not raw:
            return None
        return SaveMeta(game_id=game_id, **raw)

    def get_bytes(self, game_id: str) -> bytes | None:
        path = self.save_path(game_id)
        if not path.exists():
            return None
        return path.read_bytes()

    def _backup_existing(self, game_id: str, existing_meta: SaveMeta | None) -> None:
        if not self.keep_history:
            return
        existing_file = self.save_path(game_id)
        if not existing_file.exists() or not existing_meta:
            return
        stamp_src = existing_meta.server_updated_at or existing_meta.last_modified_utc
        stamp = stamp_src.replace(":", "-")
        history_path = self.history_dir(game_id) / f"{stamp}-{existing_meta.sha256[:8]}.sav"
        shutil.copy2(existing_file, history_path)

    def upsert(self, game_id: str, data: bytes, incoming: SaveMeta, force: bool = False) -> tuple[SaveMeta, bool]:
        """
        Returns: (effective_meta, conflict_detected)
        """
        computed = hashlib.sha256(data).hexdigest()
        if computed != incoming.sha256:
            raise ValueError("sha256 mismatch")

        with self._lock:
            index = self._read_index()
            existing_raw = index.get(game_id)
            existing = SaveMeta(game_id=game_id, **existing_raw) if existing_raw else None
            conflict = False

            if existing:
                if not force:
                    existing_time = parse_utc(existing.last_modified_utc)
                    incoming_time = parse_utc(incoming.last_modified_utc)

                    # equal timestamp but different payload should be preserved as conflict.
                    if incoming_time == existing_time and existing.sha256 != incoming.sha256:
                        conflict = True
                        incoming.conflict = True

                    # older upload does not replace newer remote.
                    if incoming_time < existing_time:
                        return existing, conflict

                self._backup_existing(game_id, existing)

            target = self.save_path(game_id)
            tmp = target.with_suffix(".tmp")
            tmp.write_bytes(data)
            os.replace(tmp, target)

            incoming.server_updated_at = utc_now_iso()
            incoming.version = (existing.version + 1) if existing else 1
            index[game_id] = incoming.model_dump(exclude={"game_id"})
            self._write_index(index)
            return incoming, conflict

    def resolve_conflict(self, game_id: str) -> bool:
        with self._lock:
            index = self._read_index()
            raw = index.get(game_id)
            if not raw:
                return False
            if not raw.get("conflict", False):
                return True
            raw["conflict"] = False
            index[game_id] = raw
            self._write_index(index)
            return True
