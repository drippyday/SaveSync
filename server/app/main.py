from __future__ import annotations

import os
from pathlib import Path

from dotenv import load_dotenv
from fastapi import Body, Depends, FastAPI, HTTPException, Query, Response, status
from fastapi.responses import JSONResponse

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


@app.put("/save/{game_id}", dependencies=[Depends(require_api_key)])
def put_save(
    game_id: str,
    body: bytes = Body(..., media_type="application/octet-stream"),
    last_modified_utc: str = Query(...),
    sha256: str = Query(...),
    size_bytes: int = Query(...),
    filename_hint: str | None = Query(default=None),
    platform_source: str | None = Query(default=None),
    force: bool = Query(default=False),
) -> JSONResponse:
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
        effective, conflict = store.upsert(game_id, body, meta, force=force)
    except ValueError as exc:
        raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    return JSONResponse(
        status_code=200,
        content={
            "game_id": game_id,
            "saved": True,
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
