import hashlib
import json
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


def test_root_lists_entrypoints(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    r = client.get("/")
    assert r.status_code == 200
    body = r.json()
    assert body["service"] == "GBAsync"
    assert body["health"] == "/health"
    assert "/admin" in body.get("admin_ui", "")


def test_root_redirects_browser_to_admin_ui(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    r = client.get(
        "/",
        headers={"Accept": "text/html,application/xhtml+xml;q=0.9,*/*;q=0.8"},
        follow_redirects=False,
    )
    assert r.status_code == 302
    loc = r.headers.get("location") or ""
    assert loc.endswith("/admin/ui/")


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


def test_save_store_repairs_empty_index_file(tmp_path: Path) -> None:
    index_path = tmp_path / "index.json"
    index_path.write_text("", encoding="utf-8")
    SaveStore(save_root=tmp_path / "saves", history_root=tmp_path / "history", index_path=index_path)
    assert json.loads(index_path.read_text(encoding="utf-8")) == {}


def test_list_saves_omits_index_without_blob(tmp_path: Path) -> None:
    """Orphan index rows must not appear in /saves (avoids client ERROR(download) on GET)."""
    client = _make_client(tmp_path)
    index_path = tmp_path / "index.json"
    ghost = {
        "ghost": {
            "last_modified_utc": "2026-01-01T00:00:00+00:00",
            "sha256": hashlib.sha256(b"").hexdigest(),
            "size_bytes": 0,
            "filename_hint": None,
            "platform_source": None,
            "conflict": False,
            "version": 1,
        }
    }
    index_path.write_text(json.dumps(ghost), encoding="utf-8")
    listed = client.get("/saves", headers=_auth_headers())
    assert listed.status_code == 200
    assert listed.json()["saves"] == []


def test_rom_sha1_routes_legacy_alias_to_canonical(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    rom_sha1 = "a" * 40

    first = b"one"
    first_params = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": hashlib.sha256(first).hexdigest(),
        "size_bytes": len(first),
        "filename_hint": "Pokemon Emerald.sav",
        "platform_source": "test",
        "rom_sha1": rom_sha1,
    }
    put_first = client.put("/save/pokemon-emer-bpee", params=first_params, content=first, headers=_auth_headers())
    assert put_first.status_code == 200
    assert put_first.json()["game_id"] == "pokemon-emer-bpee"

    second = b"two"
    second_params = {
        "last_modified_utc": "2026-03-18T21:00:00+00:00",
        "sha256": hashlib.sha256(second).hexdigest(),
        "size_bytes": len(second),
        "filename_hint": "Emerald.sav",
        "platform_source": "test",
        "rom_sha1": rom_sha1,
    }
    put_second = client.put("/save/emerald", params=second_params, content=second, headers=_auth_headers())
    assert put_second.status_code == 200
    assert put_second.json()["game_id"] == "pokemon-emer-bpee"

    get_alias = client.get("/save/emerald", headers=_auth_headers())
    assert get_alias.status_code == 200
    assert get_alias.content == second

    listed = client.get("/saves", headers=_auth_headers())
    assert listed.status_code == 200
    ids = [item["game_id"] for item in listed.json()["saves"]]
    assert ids == ["pokemon-emer-bpee"]


def test_admin_dashboard_404_when_disabled(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.delenv("GBASYNC_ADMIN_PASSWORD", raising=False)
    monkeypatch.delenv("SAVESYNC_ADMIN_PASSWORD", raising=False)
    client = _make_client(tmp_path)
    assert client.get("/admin/api/dashboard").status_code == 404


def test_admin_dashboard_auth(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("GBASYNC_ADMIN_PASSWORD", "admin-test-pw")
    monkeypatch.setenv("API_KEY", "test-api-key-123")
    client = _make_client(tmp_path)
    assert client.get("/admin/api/dashboard").status_code == 401
    r = client.get("/admin/api/dashboard", headers={"X-API-Key": "test-api-key-123"})
    assert r.status_code == 200
    body = r.json()
    assert "save_count" in body
    assert body["index_path"]
    assert "history_max_versions_per_game" in body


def test_admin_put_save_roundtrip(tmp_path: Path, monkeypatch) -> None:
    """Browser admin uploads use PUT /admin/api/save/{game_id} with session or X-API-Key."""
    monkeypatch.setenv("GBASYNC_ADMIN_PASSWORD", "admin-test-pw")
    monkeypatch.setenv("API_KEY", "test-api-key-123")
    client = _make_client(tmp_path)
    data = b"admin-web-upload"
    sha = hashlib.sha256(data).hexdigest()
    params = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": sha,
        "size_bytes": len(data),
        "filename_hint": "picked.sav",
        "platform_source": "admin-web",
        "force": "true",
    }
    h = {"X-API-Key": "test-api-key-123"}
    put = client.put("/admin/api/save/upload-gid", params=params, content=data, headers=h)
    assert put.status_code == 200
    assert put.json()["applied"] is True
    get = client.get("/save/upload-gid", headers=h)
    assert get.status_code == 200
    assert get.content == data


def test_admin_put_save_without_client_sha256(tmp_path: Path, monkeypatch) -> None:
    """Admin UI on non-HTTPS LAN may omit sha256; server hashes the body."""
    monkeypatch.setenv("GBASYNC_ADMIN_PASSWORD", "admin-test-pw")
    monkeypatch.setenv("API_KEY", "test-api-key-123")
    client = _make_client(tmp_path)
    data = b"no-client-sha"
    sha = hashlib.sha256(data).hexdigest()
    params = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "size_bytes": len(data),
        "platform_source": "admin-web",
        "force": "true",
    }
    h = {"X-API-Key": "test-api-key-123"}
    put = client.put("/admin/api/save/no-sha-gid", params=params, content=data, headers=h)
    assert put.status_code == 200
    assert put.json()["applied"] is True
    assert put.json()["effective_meta"]["sha256"] == sha


def test_admin_settings_and_routing_put(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("GBASYNC_ADMIN_PASSWORD", "admin-test-pw")
    monkeypatch.setenv("API_KEY", "test-api-key-123")
    client = _make_client(tmp_path)
    h = {"X-API-Key": "test-api-key-123"}
    r = client.patch("/admin/api/settings", json={"history_max_versions_per_game": 9}, headers=h)
    assert r.status_code == 200
    assert r.json()["history_max_versions_per_game"] == 9
    assert client.get("/admin/api/dashboard", headers=h).json()["history_max_versions_per_game"] == 9
    rom_hex = "a" * 40
    put = client.put(
        "/admin/api/index-routing",
        json={
            "aliases": {"legacy-id": "canonical-game"},
            "rom_sha1": {rom_hex: "canonical-game"},
            "tombstones": {"dead-id": "canonical-game"},
        },
        headers=h,
    )
    assert put.status_code == 200
    idx = client.get("/admin/api/index-state", headers=h).json()
    assert idx["aliases"]["legacy-id"] == "canonical-game"
    assert idx["rom_sha1"][rom_hex] == "canonical-game"
    assert idx["tombstones"]["dead-id"] == "canonical-game"


def test_display_name_patch_and_list(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    data = b"one"
    sha = hashlib.sha256(data).hexdigest()
    params = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": sha,
        "size_bytes": len(data),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    assert client.put("/save/hist-game", params=params, content=data, headers=_auth_headers()).status_code == 200
    r = client.patch("/save/hist-game/meta", json={"display_name": "pre boss"}, headers=_auth_headers())
    assert r.status_code == 200
    listed = client.get("/saves", headers=_auth_headers())
    assert listed.status_code == 200
    rows = listed.json()["saves"]
    assert any(s.get("game_id") == "hist-game" and s.get("display_name") == "pre boss" for s in rows)


def test_history_list_and_restore(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    v1 = b"v1-bytes-data"
    v2 = b"v2-bytes-data"
    p1 = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": hashlib.sha256(v1).hexdigest(),
        "size_bytes": len(v1),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    p2 = {
        "last_modified_utc": "2026-03-18T21:00:00+00:00",
        "sha256": hashlib.sha256(v2).hexdigest(),
        "size_bytes": len(v2),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    assert client.put("/save/restore-me", params=p1, content=v1, headers=_auth_headers()).status_code == 200
    assert client.put("/save/restore-me", params=p2, content=v2, headers=_auth_headers()).status_code == 200
    h = client.get("/save/restore-me/history", headers=_auth_headers())
    assert h.status_code == 200
    entries = h.json()["entries"]
    assert entries
    fn = entries[0]["filename"]
    assert client.get("/save/restore-me", headers=_auth_headers()).content == v2
    r = client.post("/save/restore-me/restore", json={"filename": fn}, headers=_auth_headers())
    assert r.status_code == 200
    assert client.get("/save/restore-me", headers=_auth_headers()).content == v1


def test_history_revision_label(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    v1 = b"one"
    v2 = b"two"
    p1 = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": hashlib.sha256(v1).hexdigest(),
        "size_bytes": len(v1),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    p2 = {
        "last_modified_utc": "2026-03-18T21:00:00+00:00",
        "sha256": hashlib.sha256(v2).hexdigest(),
        "size_bytes": len(v2),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    assert client.put("/save/rev-lab", params=p1, content=v1, headers=_auth_headers()).status_code == 200
    assert client.put("/save/rev-lab", params=p2, content=v2, headers=_auth_headers()).status_code == 200
    h = client.get("/save/rev-lab/history", headers=_auth_headers())
    assert h.status_code == 200
    fn = h.json()["entries"][0]["filename"]
    patch = client.patch(
        "/save/rev-lab/history/revision",
        json={"filename": fn, "display_name": "before gym"},
        headers=_auth_headers(),
    )
    assert patch.status_code == 200
    h2 = client.get("/save/rev-lab/history", headers=_auth_headers())
    assert h2.json()["entries"][0]["display_name"] == "before gym"


def test_history_revision_keep_and_list(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    v1 = b"one"
    v2 = b"two"
    p1 = {
        "last_modified_utc": "2026-03-17T21:00:00+00:00",
        "sha256": hashlib.sha256(v1).hexdigest(),
        "size_bytes": len(v1),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    p2 = {
        "last_modified_utc": "2026-03-18T21:00:00+00:00",
        "sha256": hashlib.sha256(v2).hexdigest(),
        "size_bytes": len(v2),
        "filename_hint": "t.sav",
        "platform_source": "test",
    }
    assert client.put("/save/keep-game", params=p1, content=v1, headers=_auth_headers()).status_code == 200
    assert client.put("/save/keep-game", params=p2, content=v2, headers=_auth_headers()).status_code == 200
    h = client.get("/save/keep-game/history", headers=_auth_headers())
    assert h.status_code == 200
    fn = h.json()["entries"][0]["filename"]
    assert h.json()["entries"][0].get("keep") is False
    r = client.patch(
        "/save/keep-game/history/revision/keep",
        json={"filename": fn, "keep": True},
        headers=_auth_headers(),
    )
    assert r.status_code == 200
    h2 = client.get("/save/keep-game/history", headers=_auth_headers())
    assert h2.json()["entries"][0]["keep"] is True
    r2 = client.patch(
        "/save/keep-game/history/revision/keep",
        json={"filename": fn, "keep": False},
        headers=_auth_headers(),
    )
    assert r2.status_code == 200
    h3 = client.get("/save/keep-game/history", headers=_auth_headers())
    assert h3.json()["entries"][0]["keep"] is False


def test_save_order_admin_and_list_saves(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    for gid in ("b-game", "a-game"):
        data = gid.encode()
        sha = hashlib.sha256(data).hexdigest()
        params = {
            "last_modified_utc": "2026-03-17T21:00:00+00:00",
            "sha256": sha,
            "size_bytes": len(data),
            "filename_hint": f"{gid}.sav",
            "platform_source": "test",
        }
        assert client.put(f"/save/{gid}", params=params, content=data, headers=_auth_headers()).status_code == 200
    r1 = client.get("/saves", headers=_auth_headers())
    assert r1.status_code == 200
    ids_default = [x["game_id"] for x in r1.json()["saves"]]
    assert ids_default == ["b-game", "a-game"]
    perm = ["a-game", "b-game"]
    put = client.put("/admin/api/save-order", json={"game_ids": perm}, headers=_auth_headers())
    assert put.status_code == 200
    r2 = client.get("/saves", headers=_auth_headers())
    body = r2.json()["saves"]
    assert [x["game_id"] for x in body] == perm
    assert [x["list_order"] for x in body] == [0, 1]


def test_list_saves_pagination_and_total(tmp_path: Path) -> None:
    client = _make_client(tmp_path)
    for gid, payload in (("pag-a", b"a"), ("pag-b", b"b"), ("pag-c", b"c")):
        sha = hashlib.sha256(payload).hexdigest()
        params = {
            "last_modified_utc": "2026-03-17T21:00:00+00:00",
            "sha256": sha,
            "size_bytes": len(payload),
            "filename_hint": f"{gid}.sav",
            "platform_source": "test",
        }
        assert client.put(f"/save/{gid}", params=params, content=payload, headers=_auth_headers()).status_code == 200
    full = client.get("/saves", headers=_auth_headers())
    assert full.status_code == 200
    j = full.json()
    assert j["total"] == 3
    assert len(j["saves"]) == 3
    page = client.get("/saves", headers=_auth_headers(), params={"limit": 2, "offset": 1})
    assert page.status_code == 200
    jp = page.json()
    assert jp["total"] == 3
    assert len(jp["saves"]) == 2


def test_put_save_rejects_oversized_payload(tmp_path: Path) -> None:
    old = main_mod._MAX_SAVE_UPLOAD_BYTES
    main_mod._MAX_SAVE_UPLOAD_BYTES = 32
    try:
        client = _make_client(tmp_path)
        data = b"a" * 64
        sha = hashlib.sha256(data).hexdigest()
        params = {
            "last_modified_utc": "2026-03-17T21:00:00+00:00",
            "sha256": sha,
            "size_bytes": len(data),
            "filename_hint": "t.sav",
            "platform_source": "test",
        }
        r = client.put("/save/huge", params=params, content=data, headers=_auth_headers())
        assert r.status_code == 413
    finally:
        main_mod._MAX_SAVE_UPLOAD_BYTES = old
