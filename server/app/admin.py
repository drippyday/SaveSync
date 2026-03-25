"""Browser admin UI API (session cookie + optional X-API-Key). Static assets live in admin-web/static."""

from __future__ import annotations

import hashlib
import hmac
import json
import logging
import os
import secrets
from pathlib import Path
from typing import Annotated, Any

from fastapi import APIRouter, Body, Depends, HTTPException, Query, Request, status
from fastapi.responses import JSONResponse, RedirectResponse, Response
from pydantic import BaseModel, Field

from .models import (
    AdminSettingsPatch,
    DisplayNamePatch,
    HistoryEntry,
    HistoryListResponse,
    IndexRoutingPut,
    RestoreRequest,
    RevisionKeepPatch,
    RevisionLabelPatch,
    SaveMeta,
    SaveOrderPut,
)
from .storage import SaveStore

_log = logging.getLogger("gbasync.admin")

router = APIRouter(prefix="/admin", tags=["admin"])


def _env(name: str, default: str = "") -> str:
    if name.startswith("GBASYNC_"):
        legacy = "SAVESYNC_" + name[len("GBASYNC_") :]
        v = os.getenv(name, "").strip()
        if v:
            return v
        return os.getenv(legacy, default).strip()
    return os.getenv(name, default).strip()


def _admin_password_configured() -> bool:
    return bool(_env("GBASYNC_ADMIN_PASSWORD"))


def _admin_session_token() -> str:
    """Deterministic cookie value derived from password + secret (no server-side session store)."""
    secret = _env("GBASYNC_ADMIN_SECRET") or _env("API_KEY")
    pw = _env("GBASYNC_ADMIN_PASSWORD")
    if not secret or not pw:
        return ""
    return hmac.new(secret.encode("utf-8"), pw.encode("utf-8"), hashlib.sha256).hexdigest()


def _require_admin_enabled() -> None:
    if not _admin_password_configured():
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="admin UI disabled (set GBASYNC_ADMIN_PASSWORD)")


def require_admin(request: Request) -> bool:
    _require_admin_enabled()
    token = _admin_session_token()
    if not token:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="admin misconfigured")
    cookie = request.cookies.get("gbasync_admin_session")
    if cookie and secrets.compare_digest(cookie, token):
        return True
    api = (request.headers.get("X-API-Key") or "").strip()
    api_expected = _env("API_KEY")
    if api_expected and secrets.compare_digest(api, api_expected.strip()):
        return True
    raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="unauthorized")


AdminDep = Annotated[bool, Depends(require_admin)]


class LoginBody(BaseModel):
    password: str = Field(..., min_length=1)


def _get_store(request: Request) -> SaveStore:
    # Late import to avoid circular import at module load
    from . import main as main_mod

    return main_mod.store


@router.get("", include_in_schema=False)
def admin_root_redirect() -> RedirectResponse:
    return RedirectResponse(url="/admin/ui/", status_code=302)


@router.get("/api/me")
def admin_me(request: Request) -> dict[str, Any]:
    if not _admin_password_configured():
        return {"admin_enabled": False, "authenticated": False}
    token = _admin_session_token()
    if not token:
        return {"admin_enabled": True, "authenticated": False, "misconfigured": True}
    cookie = request.cookies.get("gbasync_admin_session")
    authed = bool(cookie and secrets.compare_digest(cookie, token))
    if not authed:
        api = (request.headers.get("X-API-Key") or "").strip()
        api_expected = _env("API_KEY")
        if api_expected and secrets.compare_digest(api, api_expected.strip()):
            authed = True
    return {"admin_enabled": True, "authenticated": authed, "misconfigured": False}


@router.post("/api/login")
def admin_login(body: LoginBody) -> JSONResponse:
    _require_admin_enabled()
    token = _admin_session_token()
    if not token:
        raise HTTPException(status_code=status.HTTP_503_SERVICE_UNAVAILABLE, detail="admin misconfigured")
    expected_pw = _env("GBASYNC_ADMIN_PASSWORD")
    if not secrets.compare_digest(
        hashlib.sha256(body.password.encode("utf-8")).hexdigest(),
        hashlib.sha256(expected_pw.encode("utf-8")).hexdigest(),
    ):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="invalid password")
    response = JSONResponse(status_code=200, content={"ok": True})
    response.set_cookie(
        key="gbasync_admin_session",
        value=token,
        httponly=True,
        samesite="lax",
        max_age=86400 * 30,
        path="/admin",
    )
    return response


@router.post("/api/logout")
def admin_logout() -> JSONResponse:
    r = JSONResponse(status_code=200, content={"ok": True})
    r.delete_cookie("gbasync_admin_session", path="/admin")
    return r


@router.get("/api/dashboard")
def admin_dashboard(_: AdminDep, request: Request) -> dict[str, Any]:
    store = _get_store(request)
    saves = store.list_saves()
    conflicts = store.list_conflicts()
    mode = _env("GBASYNC_DROPBOX_MODE", "off").lower()
    return {
        "dropbox_mode": mode,
        "save_count": len(saves),
        "conflict_count": len(conflicts),
        "index_path": str(store.index_path),
        "save_root": str(store.save_root),
        "history_root": str(store.history_root),
        "history_max_versions_per_game": store.history_max_per_game,
        "summary": f"{len(saves)} save(s), {len(conflicts)} conflict(s), Dropbox mode={mode!r}, "
        f"keep up to {store.history_max_per_game or 'unlimited'} history file(s) per game.",
    }


@router.get("/api/index-state")
def admin_index_state(_: AdminDep, request: Request) -> dict[str, Any]:
    store = _get_store(request)
    routing = store.export_index_routing()
    return {
        "aliases": routing["aliases"],
        "rom_sha1": routing["rom_sha1"],
        "tombstones": routing["tombstones"],
    }


@router.patch("/api/settings")
def admin_patch_settings(_: AdminDep, request: Request, body: AdminSettingsPatch) -> JSONResponse:
    store = _get_store(request)
    if body.history_max_versions_per_game is not None:
        try:
            store.set_history_max_per_game(body.history_max_versions_per_game)
        except ValueError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    _log.info("admin settings history_max=%s", store.history_max_per_game)
    return JSONResponse(
        status_code=200,
        content={"ok": True, "history_max_versions_per_game": store.history_max_per_game},
    )


@router.put("/api/index-routing")
def admin_put_index_routing(_: AdminDep, request: Request, body: IndexRoutingPut) -> JSONResponse:
    store = _get_store(request)
    try:
        store.replace_routing_maps(body.aliases, body.rom_sha1, body.tombstones)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    _log.info("admin replace_routing_maps")
    return JSONResponse(status_code=200, content={"ok": True})


@router.get("/api/slot-map")
def admin_slot_map(_: AdminDep) -> dict[str, Any]:
    raw = _env("GBASYNC_SLOT_MAP_PATH") or _env("SAVESYNC_SLOT_MAP_PATH")
    if not raw:
        return {"configured": False, "path": None, "json": None}
    path = Path(raw).expanduser()
    if not path.is_file():
        return {"configured": True, "path": str(path), "json": None, "error": "file not found"}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return {"configured": True, "path": str(path), "json": None, "error": str(exc)}
    return {"configured": True, "path": str(path), "json": data}


@router.get("/api/saves")
def admin_list_saves(_: AdminDep, request: Request) -> dict[str, Any]:
    from .models import SaveListResponse

    store = _get_store(request)
    rows = store.list_saves()
    return SaveListResponse(saves=rows, total=len(rows)).model_dump()


@router.get("/api/save/{game_id}/download")
def admin_download_save(_: AdminDep, request: Request, game_id: str) -> Response:
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    data = store.get_bytes(game_id)
    if data is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not found")
    safe = game_id.replace('"', "")
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={"Content-Disposition": f'attachment; filename="{safe}.sav"'},
    )


@router.put("/api/save/{game_id}")
def admin_put_save(
    _: AdminDep,
    request: Request,
    game_id: str,
    body: bytes = Body(..., media_type="application/octet-stream"),
    last_modified_utc: str = Query(...),
    sha256: str | None = Query(
        default=None,
        description="Optional; if omitted the server hashes the body (for admin UI on non-HTTPS LAN without Web Crypto).",
    ),
    size_bytes: int = Query(...),
    rom_sha1: str | None = Query(default=None),
    filename_hint: str | None = Query(default=None),
    platform_source: str | None = Query(default=None),
    client_clock_utc: str | None = Query(default=None),
    force: bool = Query(default=True),
) -> JSONResponse:
    """
    Same semantics as ``PUT /save/{game_id}`` (client upload), but under ``/admin/api`` so the
    static admin UI can upload bytes with the session cookie. Default ``force=true`` avoids
    timestamp-only rejections when replacing from disk.
    """
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    if len(body) != size_bytes:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="size_bytes mismatch")
    computed_sha = hashlib.sha256(body).hexdigest()
    if sha256 is not None and str(sha256).strip().lower() != computed_sha:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="sha256 mismatch")
    sha256 = computed_sha
    if _client_debug_logging_enabled() and client_clock_utc:
        _log.info(
            "admin put_save client_clock game_id=%s last_modified_utc=%s client_clock_utc=%s platform_source=%s",
            game_id,
            last_modified_utc,
            client_clock_utc,
            platform_source,
        )
    meta = SaveMeta(
        game_id=game_id,
        last_modified_utc=last_modified_utc,
        sha256=sha256,
        size_bytes=size_bytes,
        rom_sha1=rom_sha1,
        filename_hint=filename_hint,
        platform_source=platform_source,
    )
    try:
        effective, conflict, applied, canonical_game_id = store.upsert(game_id, body, meta, force=force)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    from . import main as main_mod

    if applied and main_mod._dropbox_sync_on_upload_enabled():
        main_mod._schedule_dropbox_sync_after_upload()
    _log.info("admin put_save game_id=%s canonical=%s applied=%s", game_id, canonical_game_id, applied)
    return JSONResponse(
        status_code=200,
        content={
            "game_id": canonical_game_id,
            "requested_game_id": game_id,
            "saved": True,
            "applied": applied,
            "conflict": conflict,
            "effective_meta": effective.model_dump(),
        },
    )


def _client_debug_logging_enabled() -> bool:
    return os.getenv("SAVE_SYNC_LOG_CLIENT_DEBUG", "").lower() in ("1", "true", "yes")


@router.get("/api/save/{game_id}/history")
def admin_save_history(_: AdminDep, request: Request, game_id: str) -> dict[str, Any]:
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    raw = store.list_history(game_id)
    return HistoryListResponse(entries=[HistoryEntry(**e) for e in raw]).model_dump()


@router.post("/api/save/{game_id}/restore")
def admin_restore_history(_: AdminDep, request: Request, game_id: str, body: RestoreRequest) -> JSONResponse:
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    try:
        effective = store.restore_from_history(game_id, body.filename)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail=str(exc)) from exc
    from . import main as main_mod

    if main_mod._dropbox_sync_on_upload_enabled():
        main_mod._schedule_dropbox_sync_after_upload()
    _log.info("admin restore_from_history game_id=%s filename=%s", game_id, body.filename)
    return JSONResponse(
        status_code=200,
        content={"ok": True, "effective_meta": effective.model_dump()},
    )


@router.patch("/api/save/{game_id}/history/revision")
def admin_patch_history_revision(_: AdminDep, request: Request, game_id: str, body: RevisionLabelPatch) -> JSONResponse:
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    try:
        ok = store.set_history_revision_display_name(game_id, body.filename, body.display_name)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    if not ok:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="save or history file not found")
    _log.info("admin set_history_revision_display_name game_id=%s filename=%s", game_id, body.filename)
    return JSONResponse(status_code=200, content={"ok": True})


@router.patch("/api/save/{game_id}/history/revision/keep")
def admin_patch_history_keep(_: AdminDep, request: Request, game_id: str, body: RevisionKeepPatch) -> JSONResponse:
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    try:
        ok = store.set_history_revision_keep(game_id, body.filename, body.keep)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    if not ok:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="save or history file not found")
    _log.info("admin set_history_revision_keep game_id=%s filename=%s keep=%s", game_id, body.filename, body.keep)
    return JSONResponse(status_code=200, content={"ok": True})


@router.put("/api/save-order")
def admin_put_save_order(_: AdminDep, request: Request, body: SaveOrderPut) -> JSONResponse:
    store = _get_store(request)
    try:
        store.set_save_order(body.game_ids)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
    _log.info("admin set_save_order count=%s", len(body.game_ids))
    return JSONResponse(status_code=200, content={"ok": True})


@router.patch("/api/save/{game_id}/meta")
def admin_patch_save_meta(_: AdminDep, request: Request, game_id: str, body: DisplayNamePatch) -> JSONResponse:
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    if not store.set_display_name(game_id, body.display_name):
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not found")
    _log.info("admin set_display_name game_id=%s", game_id)
    return JSONResponse(status_code=200, content={"ok": True})


@router.get("/api/conflicts")
def admin_list_conflicts(_: AdminDep, request: Request) -> dict[str, Any]:
    from .models import SaveListResponse

    store = _get_store(request)
    return SaveListResponse(saves=store.list_conflicts()).model_dump()


@router.post("/api/dropbox/sync-once")
def admin_dropbox_sync_once(_: AdminDep) -> JSONResponse:
    mode = _env("GBASYNC_DROPBOX_MODE", "off").lower()
    if mode not in ("delta_api", "plain"):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="GBASYNC_DROPBOX_MODE must be delta_api or plain",
        )
    from . import main as main_mod

    main_mod._run_dropbox_bridge_once()
    return JSONResponse(status_code=200, content={"ok": True, "mode": mode})


@router.post("/api/resolve/{game_id}")
def admin_resolve(_: AdminDep, request: Request, game_id: str) -> JSONResponse:
    store = _get_store(request)
    if not store.resolve_conflict(game_id):
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not found")
    _log.info("admin resolve_conflict game_id=%s", game_id)
    return JSONResponse(status_code=200, content={"game_id": game_id, "resolved": True})


@router.delete("/api/save/{game_id}")
def admin_delete_save(_: AdminDep, request: Request, game_id: str) -> JSONResponse:
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    store = _get_store(request)
    if not store.remove(game_id):
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not in index")
    _log.info("admin delete_save game_id=%s", game_id)
    return JSONResponse(status_code=200, content={"game_id": game_id, "deleted": True})
