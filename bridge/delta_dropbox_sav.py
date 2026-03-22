#!/usr/bin/env python3
"""
List / export / import plain .sav files against a Delta Emulator Dropbox folder.
Use ``verify-rom-hashes`` to confirm whether ``sha1Hash`` matches standard SHA-1 of ``Game-*-game`` bytes.

The folder is usually named "Delta Emulator" and contains Harmony JSON + binary
attachments (see DELTA_DROPBOX_FORMAT.md).

Examples:
  python3 delta_dropbox_sav.py list --delta-root "/path/to/Delta Emulator"
  python3 delta_dropbox_sav.py export --delta-root "..." --out-dir ./exported_savs
  python3 delta_dropbox_sav.py import-sav --delta-root "..." --identifier <40-hex> \\
      --sav ./my.sav --backup-dir ./backup_before_import
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import string
import unicodedata
import uuid
from collections import OrderedDict
from datetime import datetime, timedelta, timezone
from pathlib import Path

from game_id import game_id_from_rom_bytes

HEX40 = re.compile(r"^[0-9a-f]{40}$", re.I)

# macOS / Cocoa reference date for Harmony modifiedDate fields we observed
_DELTA_REF = datetime(2001, 1, 1, tzinfo=timezone.utc)


def _now_modified_date() -> float:
    return (datetime.now(timezone.utc) - _DELTA_REF).total_seconds()


def _sha1(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def _sanitize_filename(name: str) -> str:
    ascii_name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode("ascii")
    allowed = string.ascii_letters + string.digits + "._- "
    out = "".join(c if c in allowed else "-" for c in ascii_name).strip("-_. ")
    return out or "save"


def _find_game_json_paths(delta_root: Path) -> list[Path]:
    out: list[Path] = []
    for p in delta_root.iterdir():
        if not p.is_file():
            continue
        name = p.name
        if not name.startswith("Game-"):
            continue
        if name.endswith("-game") or name.endswith("-artwork") or name.endswith("-bios7") or name.endswith("-bios9") or name.endswith("-firmware"):
            continue
        if "GameCollection" in name or "BIOS" in name or "MelonDS" in name:
            continue
        stem = name[len("Game-") :]
        if not HEX40.match(stem):
            continue
        try:
            if p.read_bytes()[:1] != b"{":
                continue
        except OSError:
            continue
        out.append(p)
    return sorted(out)


def _save_blob_candidates(delta_root: Path, gid: str) -> list[Path]:
    gid = gid.lower()
    return [
        delta_root / f"GameSave-{gid}-gameSave",
        delta_root / f"gamesave-{gid}-gamesave",
        delta_root / f"GameSave-{gid}-gamesave",
        delta_root / f"gamesave-{gid}-gameSave",
    ]


def _find_save_blob_path(delta_root: Path, gid: str) -> Path | None:
    for c in _save_blob_candidates(delta_root, gid):
        if c.is_file():
            return c
    return None


def _all_save_blob_paths(delta_root: Path, gid: str) -> list[Path]:
    """Every on-disk Harmony save blob for this id (Dropbox may have several spellings)."""
    return [c for c in _save_blob_candidates(delta_root, gid) if c.is_file()]


def _remote_identifier_basename(remote_identifier: str) -> str | None:
    s = remote_identifier.strip().replace("\\", "/")
    if not s:
        return None
    bn = s.split("/")[-1].strip()
    return bn or None


def _extra_blob_paths_from_remote_identifier(
    delta_root: Path, identifier: str, meta: dict, existing_names: set[str]
) -> list[Path]:
    """If ``files[0].remoteIdentifier`` names a blob not in our candidate list, include it.

    Delta downloads the path in ``remoteIdentifier``; if we only updated a different spelling
    on disk, the app can fetch stale bytes that no longer match ``sha1Hash`` and crash.
    """
    files = meta.get("files") or []
    if not files or not isinstance(files[0], dict):
        return []
    ri = files[0].get("remoteIdentifier")
    if not isinstance(ri, str):
        return []
    bn = _remote_identifier_basename(ri)
    if not bn or identifier.lower() not in bn.lower():
        return []
    existing_lower = {n.lower() for n in existing_names}
    if bn.lower() in existing_lower:
        return []
    return [delta_root / bn]


def _dedupe_paths(paths: list[Path]) -> list[Path]:
    seen: set[str] = set()
    out: list[Path] = []
    for p in paths:
        key = str(p.resolve())
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out


def _sync_remote_identifier_to_primary_blob(meta: dict, blob_paths: list[Path]) -> None:
    """Point ``files[0].remoteIdentifier`` at the canonical blob basename we actually write."""
    if not blob_paths:
        return
    files = meta.get("files")
    if not isinstance(files, list) or not files or not isinstance(files[0], dict):
        return
    f0 = files[0]
    primary = blob_paths[0]
    ri = f0.get("remoteIdentifier")
    if isinstance(ri, str) and ri.strip():
        ri_norm = ri.strip().replace("\\", "/")
        ri_bn = ri_norm.split("/")[-1]
        for bp in blob_paths:
            if bp.name.lower() == ri_bn.lower():
                primary = bp
                break
        if "/" in ri_norm:
            parent = ri_norm[: ri_norm.rfind("/")]
            f0["remoteIdentifier"] = f"{parent}/{primary.name}"
        else:
            f0["remoteIdentifier"] = primary.name
    else:
        f0["remoteIdentifier"] = primary.name


def _primary_blob_path_from_meta(delta_root: Path, meta: dict, fallback: Path) -> Path:
    files = meta.get("files")
    if isinstance(files, list) and files and isinstance(files[0], dict):
        ri = files[0].get("remoteIdentifier")
        if isinstance(ri, str):
            bn = _remote_identifier_basename(ri)
            if bn:
                candidate = delta_root / bn
                if candidate.exists():
                    return candidate
                # Dropbox paths are case-insensitive; keep using an existing local spelling.
                for c in delta_root.iterdir():
                    if c.is_file() and c.name.lower() == bn.lower():
                        return c
                return candidate
    return fallback


def _prune_unreferenced_blob_spellings(delta_root: Path, gid: str, keep: set[str]) -> None:
    for c in _save_blob_candidates(delta_root, gid):
        if c.is_file() and c.name not in keep:
            try:
                c.unlink()
            except OSError:
                pass


def _save_json_candidates(delta_root: Path, gid: str) -> list[Path]:
    gid = gid.lower()
    return [delta_root / f"GameSave-{gid}", delta_root / f"gamesave-{gid}"]


def _find_save_json_path(delta_root: Path, gid: str) -> Path | None:
    for c in _save_json_candidates(delta_root, gid):
        if c.is_file():
            try:
                if c.read_bytes()[:1] == b"{":
                    return c
            except OSError:
                pass
    return None


def _all_save_json_paths(delta_root: Path, gid: str) -> list[Path]:
    """Every GameSave / gamesave JSON sidecar for this id (must stay in sync or Delta can crash)."""
    out: list[Path] = []
    for c in _save_json_candidates(delta_root, gid):
        if c.is_file():
            try:
                if c.read_bytes()[:1] == b"{":
                    out.append(c)
            except OSError:
                pass
    return out


def _find_rom_game_blob_path(delta_root: Path, gid: str) -> Path | None:
    """Harmony ROM attachment next to ``Game-{id}`` JSON (see DELTA_DROPBOX_FORMAT.md)."""
    g = gid.strip().lower()
    for stem in (gid, g):
        for c in (
            delta_root / f"Game-{stem}-game",
            delta_root / f"game-{stem}-game",
        ):
            if c.is_file():
                return c
    return None


def load_game_row(delta_root: Path, game_path: Path) -> dict | None:
    gid = game_path.name.removeprefix("Game-")
    try:
        game = json.loads(game_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    blob = _find_save_blob_path(delta_root, gid)
    meta = _find_save_json_path(delta_root, gid)
    rec = game.get("record") or {}
    name = rec.get("name") or gid
    rom_sha1 = (game.get("sha1Hash") or rec.get("sha1Hash") or "").lower()
    return {
        "identifier": gid,
        "name": name,
        "rom_sha1": rom_sha1,
        "game_json": game_path,
        "save_blob": blob,
        "save_json": meta,
        "save_size": blob.stat().st_size if blob else 0,
    }


def delta_modified_to_iso(meta_path: Path) -> str:
    """GameSave JSON ``modifiedDate`` (sec since 2001-01-01 UTC) → ISO string for comparisons."""
    j = json.loads(meta_path.read_text(encoding="utf-8"))
    sec = float((j.get("record") or {}).get("modifiedDate", 0))
    dt = _DELTA_REF + timedelta(seconds=sec)
    return dt.replace(microsecond=0).isoformat()


def _row_actual_rom_sha1(delta_root: Path, row: dict) -> str | None:
    rp = _find_rom_game_blob_path(delta_root, row["identifier"])
    if not rp or not rp.is_file():
        return None
    try:
        return hashlib.sha1(rp.read_bytes()).hexdigest().lower()
    except OSError:
        return None


def _prefer_row_for_sha1_index(
    delta_root: Path, key_sha: str, current: dict, incoming: dict
) -> dict:
    """When two ``Game-*`` rows claim the same index key, keep the one that truly owns that ROM."""
    if current["identifier"] == incoming["identifier"]:
        return current
    cur_rom = _row_actual_rom_sha1(delta_root, current)
    inc_rom = _row_actual_rom_sha1(delta_root, incoming)
    if inc_rom == key_sha and cur_rom != key_sha:
        return incoming
    if cur_rom == key_sha and inc_rom != key_sha:
        return current
    if incoming["identifier"].lower() == key_sha:
        return incoming
    if current["identifier"].lower() == key_sha:
        return current
    return current


def build_delta_rows_by_rom_sha1(delta_root: Path) -> dict[str, dict]:
    """Map lower-case ROM SHA-1 → row from ``load_game_row`` (only entries with save + JSON).

    Harmony often names ``Game-{id}`` and ``Game-{id}-game`` so ``id`` equals SHA-1 of the ROM
    blob, but the JSON ``sha1Hash`` field can **differ** from that blob digest (observed in real
    Dropbox exports). We index by **SHA-1 of the ``-game`` file** when present, and also by
    ``sha1Hash`` when it is 40 hex chars, so local ROM scans match either value.

    If a stray ``sha1Hash`` points at another game's ROM digest, we **do not** let that second
    title steal the index entry — otherwise ``server_delta`` can push one save into the wrong
    Harmony slot.
    """
    out: dict[str, dict] = {}
    for gp in _find_game_json_paths(delta_root):
        row = load_game_row(delta_root, gp)
        if not row or not row["save_blob"] or not row["save_json"]:
            continue
        keys: list[str] = []
        rom_path = _find_rom_game_blob_path(delta_root, row["identifier"])
        if rom_path and rom_path.is_file():
            try:
                keys.append(hashlib.sha1(rom_path.read_bytes()).hexdigest().lower())
            except OSError:
                pass
        jsha = (row.get("rom_sha1") or "").strip().lower()
        if len(jsha) == 40:
            keys.append(jsha)
        if not keys:
            continue
        for k in dict.fromkeys(keys):
            if k not in out:
                out[k] = row
            else:
                out[k] = _prefer_row_for_sha1_index(delta_root, k, out[k], row)
    return out


def augment_rom_sha1_map_from_delta_rom_blobs(rom_to_gid: dict[str, str], delta_root: Path) -> int:
    """Add ROM SHA-1 → ``game_id`` from ``Game-*-game`` bytes (authoritative digest).

    Uses SHA-1 of the blob on disk. JSON ``sha1Hash`` is not required to match (Harmony can
    store a different value than ``SHA-1(-game)`` in exports).
    """
    added = 0
    for gp in _find_game_json_paths(delta_root):
        gfile = gp.name.removeprefix("Game-")
        rom_path = _find_rom_game_blob_path(delta_root, gfile)
        if not rom_path:
            continue
        try:
            raw = rom_path.read_bytes()
        except OSError:
            continue
        blob_sha = hashlib.sha1(raw).hexdigest().lower()
        gid = game_id_from_rom_bytes(raw)
        if not gid:
            continue
        if rom_to_gid.get(blob_sha) == gid:
            continue
        rom_to_gid[blob_sha] = gid
        added += 1
    return added


def apply_bytes_to_delta(
    delta_root: Path,
    identifier: str,
    new_data: bytes,
    backup_dir: Path | None = None,
) -> None:
    """Resize/pad, write blob(s), patch every GameSave JSON sidecar (SHA-1, dates).

    Dropbox exports sometimes contain **both** ``GameSave-{id}`` and ``gamesave-{id}`` (and mixed
    ``-gameSave`` / ``-gamesave`` blobs). Updating only one copy leaves the other pointing at
    stale hashes / wrong bytes — reported to **crash Delta** on launch.

    Also aligns ``files[0].remoteIdentifier`` with the blob filename we treat as canonical (preserving
    any parent path prefix) and writes bytes to any extra path named by ``remoteIdentifier`` that was
    not among the usual on-disk spellings, so the file Delta actually downloads matches ``sha1Hash``.
    """
    identifier = identifier.strip().lower()
    if not HEX40.match(identifier):
        raise ValueError(f"bad delta identifier: {identifier}")
    meta_paths = _all_save_json_paths(delta_root, identifier)
    if not meta_paths:
        raise ValueError(f"missing meta for {identifier}")

    primary_meta = meta_paths[0]
    meta = json.loads(primary_meta.read_text(encoding="utf-8"), object_pairs_hook=OrderedDict)
    files = meta.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("GameSave JSON missing files[]")
    expected = int(files[0].get("size", 0))
    if expected <= 0:
        raise ValueError("bad expected size in GameSave JSON")

    base_blobs = _all_save_blob_paths(delta_root, identifier)
    existing_names = {p.name for p in base_blobs}
    extra = _extra_blob_paths_from_remote_identifier(delta_root, identifier, meta, existing_names)
    blob_paths = _dedupe_paths(base_blobs + extra)
    if not blob_paths:
        raise ValueError(f"missing blob for {identifier} (no on-disk save and remoteIdentifier did not name one)")

    data = new_data
    if len(data) > expected:
        # mGBA (and some other emulators) write 131088 bytes for many GBA flash games: 128 KiB
        # cartridge save + 16-byte footer (RTC / flash metadata). Delta's Harmony sidecar
        # `files[0].size` is the on-device 128 KiB blob. Trimming the tail matches what
        # Delta stores; the 16 bytes are not part of that canonical image.
        if len(data) == expected + 16 and expected == 131072:
            print(
                f"[delta-apply] trim {len(data)} -> {expected} bytes "
                f"(emulator +16 B footer vs Delta 128 KiB slot) id={identifier[:8]}…",
                flush=True,
            )
            data = data[:-16]
        # NDS retail saves are often 512 KiB (524288 B) in Delta; 3DS homebrew and some tools
        # attach a small trailing block (metadata / alignment). Keep the leading 512 KiB only.
        elif expected == 524288 and len(data) > expected and (len(data) - expected) <= 512:
            print(
                f"[delta-apply] trim {len(data)} -> {expected} bytes "
                f"(NDS tail vs Delta 512 KiB slot) id={identifier[:8]}…",
                flush=True,
            )
            data = data[:expected]
        else:
            raise ValueError(f"data {len(data)} bytes > Delta expected {expected}")
    if len(data) < expected:
        data = data + b"\xff" * (expected - len(data))

    if backup_dir:
        backup_dir.mkdir(parents=True, exist_ok=True)
        ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        for bp in blob_paths:
            if bp.is_file():
                shutil.copy2(bp, backup_dir / f"{identifier}-{bp.name}.{ts}.bak")
        for mp in meta_paths:
            shutil.copy2(mp, backup_dir / f"{identifier}-{mp.name}.{ts}.bak")

    digest = _sha1(data)
    mtime = _now_modified_date()
    if "record" in meta and isinstance(meta["record"], dict):
        meta["record"]["sha1"] = digest
        meta["record"]["modifiedDate"] = mtime
    if "files" in meta and meta["files"] and isinstance(meta["files"][0], dict):
        meta["files"][0]["sha1Hash"] = digest
        # Keep Delta-authored revision tokens when present; replacing this token has been observed
        # to destabilize some libraries even when size/hash fields are correct.
        if not meta["files"][0].get("versionIdentifier"):
            meta["files"][0]["versionIdentifier"] = str(uuid.uuid4())
    _sync_remote_identifier_to_primary_blob(meta, blob_paths)

    json_out = json.dumps(meta, separators=(",", ":"), ensure_ascii=False)
    primary_bp = _primary_blob_path_from_meta(delta_root, meta, blob_paths[0])
    primary_bp.write_bytes(data)
    keep_blob_names = {primary_bp.name}
    # Preserve any extra path explicitly named by remoteIdentifier before normalization.
    for bp in blob_paths:
        if bp.name in keep_blob_names:
            continue
        if bp.name.lower() == primary_bp.name.lower():
            bp.write_bytes(data)
            keep_blob_names.add(bp.name)
    for mp in meta_paths:
        mp.write_text(json_out, encoding="utf-8")
    _prune_unreferenced_blob_spellings(delta_root, identifier, keep_blob_names)


def cmd_verify_rom_hashes(delta_root: Path) -> int:
    """Compare each ``Game-*`` JSON ``sha1Hash`` to SHA-1 of the sibling ``Game-*-game`` blob.

    ``[match]`` means JSON equals blob digest. ``[mismatch]`` is common: Harmony often keeps
    ``Game-{id}`` / ``-game`` where ``id`` **is** ``SHA-1(blob)``, while ``sha1Hash`` in JSON is
    a different 40-hex value—GBAsync indexes by **both** so ROM linking still works.
    """
    n_ok = n_mismatch = n_miss = n_noromsha = 0
    for gp in _find_game_json_paths(delta_root):
        gfile = gp.name.removeprefix("Game-")
        try:
            game = json.loads(gp.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        rec = game.get("record") or {}
        name = rec.get("name") or gfile
        rsha = (game.get("sha1Hash") or rec.get("sha1Hash") or "").strip().lower()
        if len(rsha) != 40:
            n_noromsha += 1
            print(f"[no-sha1Hash] {name} ({gp.name})")
            continue
        rom_path = _find_rom_game_blob_path(delta_root, gfile)
        if not rom_path:
            n_miss += 1
            print(f"[missing_blob] {name}\tjson_sha1={rsha[:12]}…\t(expected Game-{gfile[:8]}…-game)")
            continue
        try:
            raw = rom_path.read_bytes()
        except OSError as exc:
            n_miss += 1
            print(f"[read_fail] {name}\t{rom_path.name}\t{exc}")
            continue
        blob_sha = hashlib.sha1(raw).hexdigest().lower()
        gid_lower = gfile.strip().lower()
        id_matches_blob = len(gid_lower) == 40 and gid_lower == blob_sha
        if blob_sha == rsha:
            n_ok += 1
            print(f"[match] {name}\t{blob_sha[:12]}…\t{rom_path.name}")
        else:
            n_mismatch += 1
            tag = "id=blob" if id_matches_blob else "id!=blob"
            print(
                f"[mismatch] {name}\tjson={rsha[:12]}…\tblob={blob_sha[:12]}…\t({tag})\t{rom_path.name}"
            )
    print(
        f"# summary: json==blob_sha1:{n_ok} json!=blob_sha1:{n_mismatch} missing:{n_miss} no_json_sha1Hash:{n_noromsha}",
        flush=True,
    )
    return 0


def cmd_list(delta_root: Path) -> int:
    rows = []
    for gp in _find_game_json_paths(delta_root):
        row = load_game_row(delta_root, gp)
        if row and row["save_blob"]:
            rows.append(row)
    for r in rows:
        sj = "yes" if r["save_json"] else "NO"
        print(f"{r['identifier']}\t{r['save_size']}\t{sj}\t{r['name']}")
    print(f"# {len(rows)} game(s) with a save blob", flush=True)
    return 0


def cmd_export(delta_root: Path, out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest: list[dict] = []
    for gp in _find_game_json_paths(delta_root):
        row = load_game_row(delta_root, gp)
        if not row or not row["save_blob"]:
            continue
        stem = _sanitize_filename(row["name"]).replace(" ", "-")
        fname = f"{stem}__{row['identifier'][:8]}.sav"
        if len(fname) > 120:
            fname = f"{row['identifier']}.sav"
        target = out_dir / fname
        shutil.copy2(row["save_blob"], target)
        manifest.append(
            {
                "delta_identifier": row["identifier"],
                "game_name": row["name"],
                "rom_sha1": row["rom_sha1"],
                "exported_filename": fname,
                "size_bytes": row["save_size"],
            }
        )
        print(f"[export] {row['name']} -> {target.name}")
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"[export] wrote {len(manifest)} save(s) and manifest.json under {out_dir}")
    return 0


def cmd_import_sav(delta_root: Path, identifier: str, sav_path: Path, backup_dir: Path | None) -> int:
    identifier = identifier.strip().lower()
    if not HEX40.match(identifier):
        raise SystemExit("identifier must be 40 hex characters (see `list` output).")
    new_data = sav_path.read_bytes()
    try:
        apply_bytes_to_delta(delta_root, identifier, new_data, backup_dir)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    blob = _find_save_blob_path(delta_root, identifier)
    meta_path = _find_save_json_path(delta_root, identifier)
    print(f"[import] wrote {blob.stat().st_size} bytes to {blob.name}")
    print(f"[import] updated {meta_path.name} (sha1 + modifiedDate + versionIdentifier)")
    print("[import] Re-upload this folder to Dropbox / let Delta sync, then verify on device.")
    return 0


def main() -> None:
    p = argparse.ArgumentParser(description="Delta Dropbox folder ↔ plain .sav")
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="Print identifier, size, name for games with saves")
    pl.add_argument("--delta-root", type=Path, required=True, help='Unzipped "Delta Emulator" folder')

    pv = sub.add_parser(
        "verify-rom-hashes",
        help="Check Game JSON sha1Hash vs SHA-1(Game-*-game) (diagnostics)",
    )
    pv.add_argument("--delta-root", type=Path, required=True)

    pe = sub.add_parser("export", help="Copy save blobs to .sav files + manifest.json")
    pe.add_argument("--delta-root", type=Path, required=True)
    pe.add_argument("--out-dir", type=Path, required=True)

    pi = sub.add_parser("import-sav", help="Overwrite Delta save blob + patch GameSave JSON")
    pi.add_argument("--delta-root", type=Path, required=True)
    pi.add_argument("--identifier", required=True, help="40-char hex from `list`")
    pi.add_argument("--sav", type=Path, required=True)
    pi.add_argument("--backup-dir", type=Path, default=None, help="Optional backup of blob + JSON before write")

    args = p.parse_args()
    delta_root = args.delta_root.expanduser().resolve()
    if not delta_root.is_dir():
        raise SystemExit(f"Not a directory: {delta_root}")

    if args.cmd == "list":
        raise SystemExit(cmd_list(delta_root))
    if args.cmd == "verify-rom-hashes":
        raise SystemExit(cmd_verify_rom_hashes(delta_root))
    if args.cmd == "export":
        raise SystemExit(cmd_export(delta_root, args.out_dir.expanduser().resolve()))
    if args.cmd == "import-sav":
        raise SystemExit(
            cmd_import_sav(
                delta_root,
                args.identifier,
                args.sav.expanduser().resolve(),
                args.backup_dir.expanduser().resolve() if args.backup_dir else None,
            )
        )


if __name__ == "__main__":
    main()
