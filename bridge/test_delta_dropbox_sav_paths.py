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
    assert not lowercase_blob.exists()

    updated = json.loads(meta_path.read_text(encoding="utf-8"))
    f0 = updated["files"][0]
    assert f0["remoteIdentifier"].endswith("/" + canonical_blob.name)
    assert f0["sha1Hash"] == hashlib.sha1(new).hexdigest()
    assert updated["record"]["sha1"] == hashlib.sha1(new).hexdigest()
    assert f0["versionIdentifier"] == "old-version"
