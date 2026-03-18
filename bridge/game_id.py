from __future__ import annotations

import json
import re
from pathlib import Path


def sanitize_game_id(raw: str) -> str:
    cleaned = re.sub(r"[^a-zA-Z0-9._-]+", "-", raw.strip().lower())
    return cleaned.strip("-") or "unknown-game"


def _decode_ascii_field(chunk: bytes) -> str:
    text = chunk.decode("ascii", errors="ignore").strip("\x00 ").strip()
    return text


def game_id_from_gba_rom(rom_path: Path) -> str | None:
    try:
        data = rom_path.read_bytes()
    except OSError:
        return None
    if len(data) < 0xB0:
        return None
    title = _decode_ascii_field(data[0xA0:0xAC])  # 12 bytes
    code = _decode_ascii_field(data[0xAC:0xB0])  # 4 bytes
    if not title and not code:
        return None
    joined = f"{title}-{code}" if code else title
    return sanitize_game_id(joined)


class GameIdResolver:
    def __init__(
        self,
        rom_dirs: list[Path] | None = None,
        rom_map_path: Path | None = None,
        rom_extensions: list[str] | None = None,
    ):
        self.rom_dirs = rom_dirs or []
        self.rom_map_path = rom_map_path
        self.rom_extensions = [ext.lower() for ext in (rom_extensions or [".gba"])]
        self._map_cache: dict[str, str] = {}
        self._map_loaded = False

    def _load_map(self) -> None:
        if self._map_loaded:
            return
        self._map_loaded = True
        if not self.rom_map_path or not self.rom_map_path.exists():
            return
        raw = json.loads(self.rom_map_path.read_text(encoding="utf-8"))
        if isinstance(raw, dict):
            self._map_cache = {str(k): str(v) for k, v in raw.items()}

    def _match_via_map(self, save_stem: str) -> Path | None:
        self._load_map()
        mapped = self._map_cache.get(save_stem)
        if not mapped:
            return None
        p = Path(mapped).expanduser()
        return p if p.exists() else None

    def _match_via_dirs(self, save_stem: str) -> Path | None:
        for base in self.rom_dirs:
            for ext in self.rom_extensions:
                candidate = base / f"{save_stem}{ext}"
                if candidate.exists():
                    return candidate
        return None

    def resolve(self, save_path: Path) -> str:
        save_stem = save_path.stem
        rom_path = self._match_via_map(save_stem) or self._match_via_dirs(save_stem)
        if rom_path:
            derived = game_id_from_gba_rom(rom_path)
            if derived:
                return derived
        return sanitize_game_id(save_stem)
