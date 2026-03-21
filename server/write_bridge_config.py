#!/usr/bin/env python3
"""Emit /tmp/gbasync-*.json from the environment (Docker sidecar)."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def _env(name: str, default: str = "") -> str:
    """Prefer GBASYNC_* names, fall back to legacy SAVESYNC_* names."""
    if name.startswith("GBASYNC_"):
        legacy = "SAVESYNC_" + name[len("GBASYNC_") :]
        v = os.environ.get(name, "").strip()
        if v:
            return v
        return os.environ.get(legacy, default).strip()
    return os.environ.get(name, default).strip()


def _rom_dirs() -> list[str]:
    raw = _env("GBASYNC_ROM_DIRS")
    if not raw:
        return []
    return [p.strip() for p in raw.split(",") if p.strip()]


def _rom_extensions() -> list[str]:
    raw = _env("GBASYNC_ROM_EXTENSIONS", ".gba,.nds")
    parts = [p.strip() for p in raw.split(",") if p.strip()]
    return [p if p.startswith(".") else f".{p}" for p in parts]


def main() -> None:
    mode = _env("GBASYNC_DROPBOX_MODE", "off").lower()
    if mode == "off":
        return

    url = _env("GBASYNC_PUBLIC_SERVER_URL", "http://127.0.0.1:8080").rstrip("/")
    key = os.environ.get("API_KEY", "")
    rmap = _env("GBASYNC_ROM_MAP_PATH")
    interval = int(
        _env(
            "GBASYNC_DROPBOX_POLL_SECONDS",
            _env("GBASYNC_DROPBOX_INTERVAL_SECONDS", "300"),
        )
    )

    if mode == "plain":
        folder = os.environ.get("DROPBOX_REMOTE_FOLDER", "").strip()
        if not folder:
            print("write_bridge_config: GBASYNC_DROPBOX_MODE=plain requires DROPBOX_REMOTE_FOLDER", file=sys.stderr)
            sys.exit(1)
        cfg: dict = {
            "server_url": url,
            "api_key": key,
            "poll_seconds": max(1, interval),
            "dropbox": {"remote_folder": folder if folder.startswith("/") else "/" + folder},
            "rom_dirs": _rom_dirs(),
            "rom_extensions": _rom_extensions(),
        }
        if rmap:
            cfg["rom_map_path"] = rmap
        Path("/tmp/gbasync-plain-bridge.json").write_text(json.dumps(cfg, indent=2), encoding="utf-8")
        print(f"[write_bridge_config] plain -> /tmp/gbasync-plain-bridge.json remote={folder!r}", flush=True)

    elif mode == "delta_api":
        folder = os.environ.get("DROPBOX_REMOTE_DELTA_FOLDER", "").strip()
        if not folder:
            print(
                "write_bridge_config: GBASYNC_DROPBOX_MODE=delta_api requires DROPBOX_REMOTE_DELTA_FOLDER",
                file=sys.stderr,
            )
            sys.exit(1)
        Path("/data/delta-bridge-backups").mkdir(parents=True, exist_ok=True)
        sync_mode = _env("GBASYNC_DELTA_SYNC_MODE", "server_delta").lower()
        if sync_mode not in ("triple", "server_delta"):
            print(
                f"write_bridge_config: GBASYNC_DELTA_SYNC_MODE must be triple or server_delta, not {sync_mode!r}",
                file=sys.stderr,
            )
            sys.exit(1)
        cfg = {
            "server_url": url,
            "api_key": key,
            "sync_mode": sync_mode,
            "local_save_dir": "/data/saves",
            "backup_dir": "/data/delta-bridge-backups",
            "delta_slot_map_path": _env("GBASYNC_DELTA_SLOT_MAP_PATH", "/data/delta-slot-map.json"),
            "dropbox": {"remote_delta_folder": folder if folder.startswith("/") else "/" + folder},
            "rom_dirs": _rom_dirs(),
            "rom_extensions": _rom_extensions(),
        }
        if rmap:
            cfg["rom_map_path"] = rmap
        ow_raw = _env("GBASYNC_SERVER_DELTA_ONE_WAY").lower()
        if ow_raw in ("0", "false", "no"):
            cfg["server_delta_one_way"] = False
        elif ow_raw in ("1", "true", "yes"):
            cfg["server_delta_one_way"] = True
        else:
            # Default on: iOS Harmony timestamps often beat device uploads, blocking server→Dropbox.
            cfg["server_delta_one_way"] = True
        min_delta_raw = _env("GBASYNC_SERVER_DELTA_MIN_DELTA_WIN_SECONDS")
        if min_delta_raw:
            try:
                cfg["server_delta_min_delta_win_seconds"] = max(0, int(min_delta_raw))
            except ValueError:
                print(
                    "write_bridge_config: GBASYNC_SERVER_DELTA_MIN_DELTA_WIN_SECONDS must be integer",
                    file=sys.stderr,
                )
                sys.exit(1)
        recent_protect_raw = _env("GBASYNC_SERVER_DELTA_RECENT_SERVER_PROTECT_SECONDS")
        if recent_protect_raw:
            try:
                cfg["server_delta_recent_server_protect_seconds"] = max(0, int(recent_protect_raw))
            except ValueError:
                print(
                    "write_bridge_config: GBASYNC_SERVER_DELTA_RECENT_SERVER_PROTECT_SECONDS must be integer",
                    file=sys.stderr,
                )
                sys.exit(1)
        Path("/tmp/gbasync-delta-api.json").write_text(json.dumps(cfg, indent=2), encoding="utf-8")
        extra = " one_way" if cfg.get("server_delta_one_way") else ""
        print(
            f"[write_bridge_config] delta_api -> /tmp/gbasync-delta-api.json remote={folder!r}{extra}",
            flush=True,
        )

    else:
        print(f"write_bridge_config: unknown GBASYNC_DROPBOX_MODE={mode!r} (off | plain | delta_api)", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
