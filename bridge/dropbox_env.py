"""Load ``.env`` and build Dropbox SDK client.

In a full GBAsync checkout, use **one** ``.env`` at the repository root (next to ``bridge/``).
In a standalone bridge zip (flat layout), put ``.env`` in the same folder as these scripts.
"""
from __future__ import annotations

import os
from pathlib import Path

_BRIDGE_DIR = Path(__file__).resolve().parent


def _dotenv_paths() -> list[Path]:
    # 1) Same directory as this file (flat bridge release zip).
    # 2) Parent directory (repo root when this file lives in bridge/).
    return [_BRIDGE_DIR / ".env", _BRIDGE_DIR.parent / ".env"]


def load_bridge_dotenv() -> None:
    """Load the first existing ``.env`` from the standard locations."""
    try:
        from dotenv import load_dotenv
    except ImportError:
        return
    for path in _dotenv_paths():
        if path.is_file():
            load_dotenv(path)
            return


def make_dropbox_client():
    """Return a ``dropbox.Dropbox`` client using env credentials.

    **Option A — generated access token (simplest for your own account):** set
    ``DROPBOX_ACCESS_TOKEN`` from the App Console → your app → **Settings** → **OAuth 2** →
    **Generated access token**. Key/secret are not required for API calls in that mode.

    **Option B — refresh token (good for cron / long-running):** set ``DROPBOX_APP_KEY``,
    ``DROPBOX_APP_SECRET``, and ``DROPBOX_REFRESH_TOKEN``. Obtain the refresh token via the
    OAuth code flow with ``token_access_type=offline`` on the authorize URL (see Dropbox OAuth guide).
    """
    import dropbox

    load_bridge_dotenv()
    access = os.environ.get("DROPBOX_ACCESS_TOKEN", "").strip()
    if access:
        return dropbox.Dropbox(oauth2_access_token=access)

    key = os.environ.get("DROPBOX_APP_KEY", "").strip()
    secret = os.environ.get("DROPBOX_APP_SECRET", "").strip()
    refresh = os.environ.get("DROPBOX_REFRESH_TOKEN", "").strip()
    if key and secret and refresh:
        return dropbox.Dropbox(oauth2_refresh_token=refresh, app_key=key, app_secret=secret)

    raise SystemExit(
        "Dropbox credentials missing. Either:\n"
        "  • Set DROPBOX_ACCESS_TOKEN (App Console → Generate access token), or\n"
        "  • Set DROPBOX_APP_KEY, DROPBOX_APP_SECRET, and DROPBOX_REFRESH_TOKEN\n"
        "See bridge/DROPBOX_SETUP.md"
    )
