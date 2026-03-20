import hashlib
import os
from pathlib import Path

from fastapi.testclient import TestClient

from app.main import app
import app.main as main_mod
from app.storage import SaveStore


def _make_client(tmp_path: Path) -> TestClient:
    main_mod.store = SaveStore(
        save_root=tmp_path / "saves",
        history_root=tmp_path / "history",
        index_path=tmp_path / "index.json",
        keep_history=True,
    )
    return TestClient(app)


def _auth_headers() -> dict[str, str]:
    key = os.getenv("API_KEY", "").strip()
    return {"X-API-Key": key} if key else {}


def test_put_get_roundtrip(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    data = b"abcd"
    sha = hashlib.sha256(data).hexdigest()
    params = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": sha,
        "size_bytes": len(data),
        "filename_hint": "Pokemon Emerald.sav",
        "platform_source": "test",
    }
    put = client.put("/save/pokemon-emerald", params=params, content=data, headers=_auth_headers())
    assert put.status_code == 200

    get = client.get("/save/pokemon-emerald", headers=_auth_headers())
    assert get.status_code == 200
    assert get.content == data


def test_conflict_list_and_resolve(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    base_ts = "2026-03-17T21:00:00+00:00"
    data_a = b"A"
    data_b = b"B"

    for payload in (data_a, data_b):
        params = {
            "last_modified_utc": base_ts,
            "sha256": hashlib.sha256(payload).hexdigest(),
            "size_bytes": len(payload),
            "filename_hint": "Zelda.sav",
            "platform_source": "test",
        }
        resp = client.put("/save/zelda", params=params, content=payload, headers=_auth_headers())
        assert resp.status_code == 200

    conflicts = client.get("/conflicts", headers=_auth_headers())
    assert conflicts.status_code == 200
    ids = [item["game_id"] for item in conflicts.json()["saves"]]
    assert "zelda" in ids

    resolved = client.post("/resolve/zelda", headers=_auth_headers())
    assert resolved.status_code == 200
    conflicts_after = client.get("/conflicts", headers=_auth_headers())
    assert conflicts_after.status_code == 200
    ids_after = [item["game_id"] for item in conflicts_after.json()["saves"]]
    assert "zelda" not in ids_after


def test_delete_save(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    data = b"x"
    sha = hashlib.sha256(data).hexdigest()
    params = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": sha,
        "size_bytes": len(data),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    assert client.put("/save/stale-test", params=params, content=data, headers=_auth_headers()).status_code == 200
    assert (tmp_path / "saves" / "stale-test.sav").is_file()

    deleted = client.delete("/save/stale-test", headers=_auth_headers())
    assert deleted.status_code == 200
    assert not (tmp_path / "saves" / "stale-test.sav").exists()

    listed = client.get("/saves", headers=_auth_headers())
    assert listed.status_code == 200
    ids = [item["game_id"] for item in listed.json()["saves"]]
    assert "stale-test" not in ids

    missing = client.delete("/save/stale-test", headers=_auth_headers())
    assert missing.status_code == 404
