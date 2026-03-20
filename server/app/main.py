from __future__ import annotations

import logging
import os
from pathlib import Path

from dotenv import load_dotenv
from fastapi import Body, Depends, FastAPI, HTTPException, Query, Response, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from .auth import require_api_key
from .models import SaveListResponse, SaveMeta
from .storage import SaveStore

load_dotenv()

SAVE_ROOT = Path(os.getenv("SAVE_ROOT", "./data/saves")).resolve()
HISTORY_ROOT = Path(os.getenv("HISTORY_ROOT", "./data/history")).resolve()
INDEX_PATH = Path(os.getenv("INDEX_PATH", "./data/index.json")).resolve()
ENABLE_VERSION_HISTORY = os.getenv("ENABLE_VERSION_HISTORY", "true").lower() == "true"

store = SaveStore(
    save_root=SAVE_ROOT,
    history_root=HISTORY_ROOT,
    index_path=INDEX_PATH,
    keep_history=ENABLE_VERSION_HISTORY,
)

app = FastAPI(title="SaveSync Server", version="0.1.0")

_log = logging.getLogger("savesync")


def _client_debug_logging_enabled() -> bool:
    return os.getenv("SAVE_SYNC_LOG_CLIENT_DEBUG", "").lower() in ("1", "true", "yes")


class ClientDebugReport(BaseModel):
    utc_iso: str
    platform: str = "unknown"
    phase: str = ""
    untrusted_local_saves: int = 0


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.get("/saves", response_model=SaveListResponse, dependencies=[Depends(require_api_key)])
def list_saves() -> SaveListResponse:
    return SaveListResponse(saves=store.list_saves())


@app.get("/conflicts", response_model=SaveListResponse, dependencies=[Depends(require_api_key)])
def list_conflicts() -> SaveListResponse:
    return SaveListResponse(saves=store.list_conflicts())


@app.get("/save/{game_id}/meta", dependencies=[Depends(require_api_key)])
def get_save_meta(game_id: str) -> SaveMeta:
    meta = store.get_meta(game_id)
    if not meta:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not found")
    return meta


@app.get("/save/{game_id}", dependencies=[Depends(require_api_key)])
def get_save(game_id: str) -> Response:
    data = store.get_bytes(game_id)
    if data is None:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not found")
    return Response(content=data, media_type="application/octet-stream")


@app.post("/debug/client-clock", dependencies=[Depends(require_api_key)])
def client_debug_clock(report: ClientDebugReport) -> dict[str, bool]:
    if _client_debug_logging_enabled():
        _log.info("client_debug_report %s", report.model_dump())
    return {"ok": True}


@app.put("/save/{game_id}", dependencies=[Depends(require_api_key)])
def put_save(
    game_id: str,
    body: bytes = Body(..., media_type="application/octet-stream"),
    last_modified_utc: str = Query(...),
    sha256: str = Query(...),
    size_bytes: int = Query(...),
    filename_hint: str | None = Query(default=None),
    platform_source: str | None = Query(default=None),
    client_clock_utc: str | None = Query(
        default=None,
        description="Client wall clock at upload time (for debugging vs last_modified_utc)",
    ),
    force: bool = Query(default=False),
) -> JSONResponse:
    if _client_debug_logging_enabled() and client_clock_utc:
        _log.info(
            "put_save client_clock game_id=%s last_modified_utc=%s client_clock_utc=%s platform_source=%s",
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
        filename_hint=filename_hint,
        platform_source=platform_source,
    )

    if len(body) != size_bytes:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="size_bytes mismatch")

    try:
        effective, conflict, applied = store.upsert(game_id, body, meta, force=force)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    return JSONResponse(
        status_code=200,
        content={
            "game_id": game_id,
            "saved": True,
            "applied": applied,
            "conflict": conflict,
            "effective_meta": effective.model_dump(),
        },
    )


@app.post("/resolve/{game_id}", dependencies=[Depends(require_api_key)])
def resolve_conflict(game_id: str) -> JSONResponse:
    resolved = store.resolve_conflict(game_id)
    if not resolved:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not found")
    return JSONResponse(status_code=200, content={"game_id": game_id, "resolved": True})


@app.delete("/save/{game_id}", dependencies=[Depends(require_api_key)])
def delete_save(game_id: str) -> JSONResponse:
    """
    Remove server metadata for ``game_id`` and delete the stored .sav blob.
    Use this when test uploads or stale index rows should disappear from ``GET /saves``.
    """
    if not game_id or "/" in game_id or "\\" in game_id or game_id.startswith("."):
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="invalid game_id")
    if not store.remove(game_id):
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Save not in index")
    return JSONResponse(status_code=200, content={"game_id": game_id, "deleted": True})
