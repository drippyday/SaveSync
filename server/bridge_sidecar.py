#!/usr/bin/env python3
"""Periodic Dropbox ↔ GBAsync sync inside the server container (background)."""
from __future__ import annotations

import os
import subprocess
import sys
import time

BRIDGE_DIR = "/app/bridge"


def _env(name: str, default: str = "") -> str:
    if name.startswith("GBASYNC_"):
        legacy = "SAVESYNC_" + name[len("GBASYNC_") :]
        v = os.environ.get(name, "").strip()
        if v:
            return v
        return os.environ.get(legacy, default).strip()
    return os.environ.get(name, default).strip()


def main() -> None:
    mode = _env("GBASYNC_DROPBOX_MODE", "off").lower()
    if mode == "off":
        return 0

    interval = max(10, int(_env("GBASYNC_DROPBOX_INTERVAL_SECONDS", "300")))
    delay = max(0, int(_env("GBASYNC_BRIDGE_START_DELAY_SECONDS", "8")))
    py = sys.executable

    print(
        f"[bridge-sidecar] mode={mode} interval={interval}s start_delay={delay}s",
        flush=True,
    )
    if delay:
        time.sleep(delay)

    while True:
        try:
            if mode == "plain":
                subprocess.run(
                    [py, f"{BRIDGE_DIR}/dropbox_bridge.py", "--config", "/tmp/gbasync-plain-bridge.json", "--once"],
                    check=False,
                )
            elif mode == "delta_api":
                subprocess.run(
                    [
                        py,
                        f"{BRIDGE_DIR}/delta_dropbox_api_sync.py",
                        "--config",
                        "/tmp/gbasync-delta-api.json",
                        "--once",
                    ],
                    check=False,
                )
            else:
                print(f"[bridge-sidecar] unknown mode {mode!r}, exiting sidecar", flush=True)
                return 1
        except Exception as exc:
            print(f"[bridge-sidecar] error: {exc}", flush=True)
        time.sleep(interval)


if __name__ == "__main__":
    raise SystemExit(main())
