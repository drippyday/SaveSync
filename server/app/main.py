from __future__ import annotations

import logging
import os
import subprocess
import sys
import threading
from pathlib import Path

from dotenv import load_dotenv
from fastapi import Body, Depends, FastAPI, HTTPException, Query, Response, status
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from .auth import require_api_key
from .models import SaveListResponse, SaveMeta
from .storage import SaveStore

_REPO_ROOT = Path(__file__).resolve().parents[2]
_BRIDGE_DIR = _REPO_ROOT / "bridge"
load_dotenv(_REPO_ROOT / ".env")

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

app = FastAPI(title="GBAsync Server", version="0.1.2")

_log = logging.getLogger("gbasync")
_dropbox_sync_timer_lock = threading.Lock()
_dropbox_sync_timer: threading.Timer | None = None
_dropbox_sync_run_lock = threading.Lock()


def _env(name: str, default: str = "") -> str:
    if name.startswith("GBASYNC_"):
        legacy = "SAVESYNC_" + name[len("GBASYNC_") :]
        v = os.getenv(name, "").strip()
        if v:
            return v
        return os.getenv(legacy, default).strip()
    return os.getenv(name, default).strip()


def _client_debug_logging_enabled() -> bool:
    return os.getenv("SAVE_SYNC_LOG_CLIENT_DEBUG", "").lower() in ("1", "true", "yes")


def _env_truthy(name: str) -> bool:
    return os.getenv(name, "").strip().lower() in ("1", "true", "yes")


def _dropbox_sync_on_upload_enabled() -> bool:
    return _env_truthy("GBASYNC_DROPBOX_SYNC_ON_UPLOAD") or _env_truthy("SAVESYNC_DROPBOX_SYNC_ON_UPLOAD")


def _dropbox_sync_debounce_seconds() -> float:
    raw = _env("GBASYNC_DROPBOX_SYNC_DEBOUNCE_SECONDS", "10")
    try:
        return max(0.0, float(raw))
    except ValueError:
        _log.warning("invalid GBASYNC_DROPBOX_SYNC_DEBOUNCE_SECONDS=%r, using 10s", raw)
        return 10.0


def _bridge_runtime_paths() -> tuple[Path, Path] | None:
    """(bridge_dir, cwd) for subprocess; None if bridge scripts not on this machine."""
    if Path("/app/bridge/delta_dropbox_api_sync.py").is_file():
        return Path("/app/bridge"), Path("/app")
    if (_BRIDGE_DIR / "delta_dropbox_api_sync.py").is_file():
        return _BRIDGE_DIR, _REPO_ROOT
    return None


def _run_dropbox_bridge_once() -> None:
    mode = _env("GBASYNC_DROPBOX_MODE", "off").lower()
    if mode == "delta_api":
        cfg_name = "/tmp/gbasync-delta-api.json"
        script_name = "delta_dropbox_api_sync.py"
    elif mode == "plain":
        cfg_name = "/tmp/gbasync-plain-bridge.json"
        script_name = "dropbox_bridge.py"
    else:
        return

    cfg_path = Path(cfg_name)
    if not cfg_path.is_file():
        _log.warning("dropbox sync skipped: %s not found (is Dropbox mode enabled in this container?)", cfg_name)
        return

    paths = _bridge_runtime_paths()
    if not paths:
        _log.warning("dropbox sync skipped: bridge scripts not found")
        return
    bridge_dir, cwd = paths
    script_path = bridge_dir / script_name
    if not script_path.is_file():
        _log.warning("dropbox sync skipped: %s missing", script_path)
        return

    env = os.environ.copy()
    prev = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = f"{bridge_dir}{os.pathsep}{prev}" if prev else str(bridge_dir)

    timeout = int(_env("GBASYNC_DROPBOX_SYNC_TIMEOUT_SECONDS", "600"))
    try:
        r = subprocess.run(
            [sys.executable, str(script_path), "--config", str(cfg_path), "--once"],
            cwd=str(cwd),
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        if r.returncode != 0:
            tail = (r.stderr or r.stdout or "").strip()[-2000:]
            _log.warning("dropbox sync exit %s: %s", r.returncode, tail or "(no output)")
    except subprocess.TimeoutExpired:
        _log.warning("dropbox sync timed out after %ss", timeout)
    except OSError as exc:
        _log.warning("dropbox sync failed: %s", exc)


def _dropbox_sync_after_upload_runner() -> None:
    if not _dropbox_sync_run_lock.acquire(blocking=False):
        # A sync run is already active; try again after another quiet window.
        _log.info("dropbox sync already running; rescheduling upload-triggered sync")
        _schedule_dropbox_sync_after_upload()
        return
    try:
        _run_dropbox_bridge_once()
    finally:
        _dropbox_sync_run_lock.release()


def _schedule_dropbox_sync_after_upload() -> None:
    delay = _dropbox_sync_debounce_seconds()
    with _dropbox_sync_timer_lock:
        global _dropbox_sync_timer
        if _dropbox_sync_timer is not None:
            _dropbox_sync_timer.cancel()
            _dropbox_sync_timer = None
        t = threading.Timer(delay, _dropbox_sync_after_upload_runner)
        t.daemon = True
        _dropbox_sync_timer = t
        t.start()


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

    resp = JSONResponse(
        status_code=200,
        content={
            "game_id": game_id,
            "saved": True,
            "applied": applied,
            "conflict": conflict,
            "effective_meta": effective.model_dump(),
        },
    )
    if applied and _dropbox_sync_on_upload_enabled():
        _schedule_dropbox_sync_after_upload()
    return resp


@app.post("/dropbox/sync-once", dependencies=[Depends(require_api_key)])
def dropbox_sync_once() -> JSONResponse:
    """
    Run one Dropbox bridge pass (``delta_api`` or ``plain``) in-process, blocking until done.
    Serialized with the sidecar via ``/tmp/gbasync-dropbox-sync.lock``.
    """
    mode = _env("GBASYNC_DROPBOX_MODE", "off").lower()
    if mode not in ("delta_api", "plain"):
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail="GBASYNC_DROPBOX_MODE must be delta_api or plain",
        )
    _run_dropbox_bridge_once()
    return JSONResponse(status_code=200, content={"ok": True, "mode": mode})


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
