"""Serialize Dropbox bridge runs (sidecar, upload hook, manual) across processes."""
from __future__ import annotations

import contextlib
from collections.abc import Iterator
from pathlib import Path

try:
    import fcntl
except ImportError:
    fcntl = None  # type: ignore[misc, assignment]

_LOCK_PATH = Path("/tmp/gbasync-dropbox-sync.lock")


@contextlib.contextmanager
def dropbox_run_lock() -> Iterator[None]:
    if fcntl is None:
        yield
        return
    _LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
    fh = open(_LOCK_PATH, "w")
    try:
        fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
        yield
    finally:
        fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
        fh.close()
