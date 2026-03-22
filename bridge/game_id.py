from __future__ import annotations

import hashlib
import json
import re
import unicodedata
from pathlib import Path


def sanitize_game_id(raw: str) -> str:
    """ASCII-ish slug for matching GBAsync ``game_id`` keys (Unicode letters -> ASCII)."""
    ascii_name = unicodedata.normalize("NFKD", raw).encode("ascii", "ignore").decode("ascii")
    cleaned = re.sub(r"[^a-zA-Z0-9._-]+", "-", ascii_name.strip().lower())
    return cleaned.strip("-") or "unknown-game"


def title_looks_like_retail_for_header(name: str, header_gid: str) -> bool:
    """True if Delta's display name looks like retail Fire Red / Emerald for this ROM header."""
    n = (name or "").lower()
    g = header_gid.lower()
    compact = n.replace(" ", "")
    if "fire" in g and "bpre" in g:
        if "firered" in compact:
            return True
        return "fire" in n and "red" in n
    if "emer" in g and "bpe" in g:
        return "emerald" in n
    return False


def remote_game_id_for_delta_title(
    name: str,
    remote: dict[str, object],
    *,
    header_hint: str | None = None,
) -> str | None:
    """Return the first server save key that matches this Delta display name.

    Tries hyphenated slug (``blazing-emerald-16``) then collapsed (``blazingemerald16``) so
    index keys without hyphens still match.

    If ``header_hint`` is set and that key exists on the server, it is used only when
    :func:`title_looks_like_retail_for_header` passes — so ``pokemon-emer-bpee`` matches
    *Pokémon: Emerald Version* but not *PokeScape* (name has no \"emerald\").
    """
    base = sanitize_game_id(name)
    candidates = [base]
    collapsed = base.replace("-", "")
    if collapsed != base:
        candidates.append(collapsed)
    # Delta retail titles often include boilerplate words that are not present in server ids
    # (e.g. "pokemon-fire-red-version" vs "firered"). Try reduced title aliases too.
    stop_words = {"pokemon", "version", "the", "game", "edition"}
    title_parts = [p for p in base.split("-") if p and p not in stop_words]
    if title_parts:
        reduced = "-".join(title_parts)
        if reduced and reduced not in candidates:
            candidates.append(reduced)
        reduced_collapsed = "".join(title_parts)
        if reduced_collapsed and reduced_collapsed not in candidates:
            candidates.append(reduced_collapsed)
    # Also compare against filename stems from remote metadata (e.g. FireRed.sav -> firered).
    remote_stem_map: dict[str, str] = {}
    for gid, meta in remote.items():
        if not isinstance(meta, dict):
            continue
        hint = str(meta.get("filename_hint") or "").strip()
        if not hint:
            continue
        stem = Path(hint).stem
        stem_slug = sanitize_game_id(stem)
        if stem_slug:
            remote_stem_map[stem_slug] = gid
            stem_collapsed = stem_slug.replace("-", "")
            if stem_collapsed:
                remote_stem_map[stem_collapsed] = gid
    for nk in candidates:
        if nk and nk != "unknown-game" and nk in remote:
            return nk
        if nk and nk in remote_stem_map:
            return remote_stem_map[nk]
    if header_hint and header_hint in remote and title_looks_like_retail_for_header(name, header_hint):
        return header_hint

    # NDS retail: ROM header uses cartridge code (e.g. ``pokemon-ss-ipge``) while 3DS/Switch uploads
    # often use a filename-based slug (e.g. ``pokemon---soulsilver-version--usa-``). When the Delta
    # display title does not slug-match the server key, align via distinctive words in ``filename_hint``.
    name_tokens = [
        w
        for w in re.split(r"[^a-z0-9]+", (name or "").lower())
        if len(w) >= 5 and w not in stop_words
    ]
    if name_tokens:
        matched: list[str] = []
        for gid, meta in remote.items():
            if not isinstance(meta, dict):
                continue
            hint = str(meta.get("filename_hint") or "").strip()
            if not hint:
                continue
            stem_l = Path(hint).stem.lower()
            if all(t in stem_l for t in name_tokens):
                matched.append(str(gid))
        if len(matched) == 1:
            return matched[0]

    return None


def _decode_ascii_field(chunk: bytes) -> str:
    text = chunk.decode("ascii", errors="ignore").strip("\x00 ").strip()
    return text


def game_id_from_gba_rom(rom_path: Path) -> str | None:
    try:
        data = rom_path.read_bytes()
    except OSError:
        return None
    return game_id_from_gba_bytes(data)


def game_id_from_gba_bytes(data: bytes) -> str | None:
    if len(data) < 0xB0:
        return None
    title = _decode_ascii_field(data[0xA0:0xAC])  # 12 bytes
    code = _decode_ascii_field(data[0xAC:0xB0])  # 4 bytes
    if not title and not code:
        return None
    joined = f"{title}-{code}" if code else title
    return sanitize_game_id(joined)


def game_id_from_nds_bytes(data: bytes) -> str | None:
    """NDS cartridge header: title @ 0x00 (12), game code @ 0x0C (4)."""
    if len(data) < 0x20:
        return None
    title = _decode_ascii_field(data[0x00:0x0C])
    code = _decode_ascii_field(data[0x0C:0x10])
    if not title and not code:
        return None
    joined = f"{title}-{code}" if code else title
    return sanitize_game_id(joined)


def game_id_from_gb_bytes(data: bytes) -> str | None:
    """DMG/GBC cartridge header: title @ 0x0134 (11 chars on CGB; up to 16 on older DMG)."""
    if len(data) < 0x0144:
        return None
    # 0x0143 is CGB compatibility flag on GBC; title is then 0x0134-0x013E (11 bytes).
    if data[0x0143] in (0x80, 0xC0):
        title = _decode_ascii_field(data[0x0134:0x013F])
    else:
        title = _decode_ascii_field(data[0x0134:0x0144])
    if not title:
        return None
    return sanitize_game_id(title)


def game_id_from_rom_bytes(data: bytes) -> str | None:
    """Derive GBAsync ``game_id`` from ROM bytes: GB/DMG when plausible, then GBA, then NDS."""
    # Retail GBA ROMs are usually 4MB+; try GB first for smaller blobs so DMG/GBC titles win.
    if len(data) >= 0x144 and len(data) < 4 * 1024 * 1024:
        gb = game_id_from_gb_bytes(data)
        if gb:
            return gb
    if len(data) >= 0xB0:
        gba = game_id_from_gba_bytes(data)
        if gba:
            return gba
    if len(data) >= 0x144:
        gb = game_id_from_gb_bytes(data)
        if gb:
            return gb
    if len(data) >= 0x20:
        return game_id_from_nds_bytes(data)
    return None


def _rom_sha1_and_game_id(rom: Path) -> tuple[str, str] | None:
    try:
        raw = rom.read_bytes()
    except OSError:
        return None
    if len(raw) < 0x20:
        return None
    h = hashlib.sha1(raw).hexdigest().lower()
    ext = rom.suffix.lower()
    gid: str | None = None
    if ext == ".nds":
        gid = game_id_from_nds_bytes(raw)
    elif ext in (".gb", ".gbc"):
        gid = game_id_from_gb_bytes(raw)
    elif len(raw) >= 0xB0:
        gid = game_id_from_gba_bytes(raw)
    if h and gid:
        return h, gid
    return None


def build_rom_sha1_to_game_id(
    rom_dirs: list[Path],
    rom_extensions: list[str],
    rom_map_path: Path | None = None,
) -> dict[str, str]:
    """Map lower-case ROM SHA-1 -> GBAsync ``game_id`` (GBA header).

    Scans ``rom_dirs`` (recursive) and optionally every ROM path listed in ``rom_map_path`` JSON.
    Uses GBA / NDS / GB cartridge headers as appropriate for each file extension.
    """
    out: dict[str, str] = {}
    seen: set[Path] = set()

    if rom_map_path and rom_map_path.exists():
        raw = json.loads(rom_map_path.read_text(encoding="utf-8"))
        if isinstance(raw, dict):
            for v in raw.values():
                p = Path(str(v)).expanduser().resolve()
                if not p.is_file() or p in seen:
                    continue
                seen.add(p)
                pair = _rom_sha1_and_game_id(p)
                if pair:
                    out[pair[0]] = pair[1]

    exts = [e.lower() if e.startswith(".") else f".{e.lower()}" for e in rom_extensions]
    for base in rom_dirs:
        if not base.is_dir():
            continue
        for ext in exts:
            for rom in base.rglob(f"*{ext}"):
                if not rom.is_file():
                    continue
                rp = rom.resolve()
                if rp in seen:
                    continue
                seen.add(rp)
                pair = _rom_sha1_and_game_id(rom)
                if pair:
                    out[pair[0]] = pair[1]
    return out


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

    def resolve_stem(self, save_stem: str) -> str:
        """Resolve ``game_id`` from a save filename stem (no filesystem path required)."""
        rom_path = self._match_via_map(save_stem) or self._match_via_dirs(save_stem)
        if rom_path:
            ext = rom_path.suffix.lower()
            if ext in (".gb", ".gbc"):
                try:
                    b = rom_path.read_bytes()
                except OSError:
                    b = b""
                gb = game_id_from_gb_bytes(b)
                if gb:
                    return gb
            derived = game_id_from_gba_rom(rom_path)
            if derived:
                return derived
            if ext == ".nds":
                try:
                    b = rom_path.read_bytes()
                except OSError:
                    b = b""
                nds = game_id_from_nds_bytes(b)
                if nds:
                    return nds
        return sanitize_game_id(save_stem)

    def resolve_rom_path(self, save_path: Path) -> Path | None:
        """ROM path for header / SHA-1 linking (``rom_map_path`` / ``rom_dirs``)."""
        return self._match_via_map(save_path.stem) or self._match_via_dirs(save_path.stem)

    def resolve(self, save_path: Path) -> str:
        return self.resolve_stem(save_path.stem)
