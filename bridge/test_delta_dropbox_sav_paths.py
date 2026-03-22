from __future__ import annotations

import hashlib
import json
from pathlib import Path

from delta_dropbox_sav import apply_bytes_to_delta


def _meta_for(identifier: str, remote_identifier: str, size: int, sha1_hex: str) -> dict:
    return {
        "sha1Hash": "placeholder-top-level",
        "type": "GameSave",
        "identifier": identifier,
        "record": {"modifiedDate": 1.0, "sha1": sha1_hex},
        "relationships": {"game": {"type": "Game", "identifier": identifier}},
        "files": [
            {
                "size": size,
                "versionIdentifier": "old-version",
                "sha1Hash": sha1_hex,
                "identifier": "gameSave",
                "remoteIdentifier": remote_identifier,
            }
        ],
    }


def test_apply_bytes_to_delta_does_not_create_case_only_duplicate_blob(tmp_path: Path) -> None:
    identifier = "41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc"
    canonical_blob = tmp_path / f"GameSave-{identifier}-gameSave"
    lowercase_blob = tmp_path / f"gamesave-{identifier}-gamesave"
    meta_path = tmp_path / f"GameSave-{identifier}"

    old = b"abcd"
    canonical_blob.write_bytes(old)
    meta_path.write_text(
        json.dumps(
            _meta_for(
                identifier=identifier,
                remote_identifier=f"/delta emulator/{lowercase_blob.name}",
                size=4,
                sha1_hex=hashlib.sha1(old).hexdigest(),
            )
        ),
        encoding="utf-8",
    )

    new = b"wxyz"
    apply_bytes_to_delta(tmp_path, identifier, new, backup_dir=None)

    assert canonical_blob.read_bytes() == new
    # On case-insensitive volumes (default macOS), Harmony spellings may alias one inode;
    # then lowercase_blob.exists() is True even though we only created canonical_blob.
    if lowercase_blob.exists():
        assert canonical_blob.samefile(lowercase_blob)

    updated = json.loads(meta_path.read_text(encoding="utf-8"))
    f0 = updated["files"][0]
    assert f0["remoteIdentifier"].endswith("/" + canonical_blob.name)
    assert f0["sha1Hash"] == hashlib.sha1(new).hexdigest()
    assert updated["record"]["sha1"] == hashlib.sha1(new).hexdigest()
    assert f0["versionIdentifier"] == "old-version"


def test_apply_bytes_to_delta_trims_mgba_16_byte_footer_for_128k_slot(tmp_path: Path) -> None:
    """mGBA often sends 131088 B; Delta Harmony expects 131072 B — keep first 128 KiB only."""
    identifier = "41cb23d8dccc8ebd7c649cd8fbb58eeace6e2fdc"
    canonical_blob = tmp_path / f"GameSave-{identifier}-gameSave"
    meta_path = tmp_path / f"GameSave-{identifier}"

    old = b"A" * 131072
    canonical_blob.write_bytes(old)
    meta_path.write_text(
        json.dumps(
            _meta_for(
                identifier=identifier,
                remote_identifier=f"/delta emulator/{canonical_blob.name}",
                size=131072,
                sha1_hex=hashlib.sha1(old).hexdigest(),
            )
        ),
        encoding="utf-8",
    )

    padded = old + b"\x00" * 16
    assert len(padded) == 131088
    apply_bytes_to_delta(tmp_path, identifier, padded, backup_dir=None)

    assert canonical_blob.read_bytes() == old
    updated = json.loads(meta_path.read_text(encoding="utf-8"))
    assert updated["files"][0]["sha1Hash"] == hashlib.sha1(old).hexdigest()


def test_apply_bytes_to_delta_trims_nds_small_overrun_for_512k_slot(tmp_path: Path) -> None:
    """3DS uploads can be 512 KiB + small tail; Delta Harmony expects exactly 524288 B."""
    identifier = "f8dc38ea20c17541a43b58c5e6d18c1732c7e582"
    canonical_blob = tmp_path / f"GameSave-{identifier}-gameSave"
    meta_path = tmp_path / f"GameSave-{identifier}"

    old = b"B" * 524288
    canonical_blob.write_bytes(old)
    meta_path.write_text(
        json.dumps(
            _meta_for(
                identifier=identifier,
                remote_identifier=f"/delta emulator/{canonical_blob.name}",
                size=524288,
                sha1_hex=hashlib.sha1(old).hexdigest(),
            )
        ),
        encoding="utf-8",
    )

    padded = old + b"\x00" * 122
    assert len(padded) == 524410
    apply_bytes_to_delta(tmp_path, identifier, padded, backup_dir=None)

    assert canonical_blob.read_bytes() == old
    updated = json.loads(meta_path.read_text(encoding="utf-8"))
    assert updated["files"][0]["sha1Hash"] == hashlib.sha1(old).hexdigest()
