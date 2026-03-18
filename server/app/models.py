from __future__ import annotations

from datetime import datetime, timezone
from pydantic import BaseModel, Field, field_validator


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


class SaveMeta(BaseModel):
    game_id: str
    last_modified_utc: str = Field(default_factory=utc_now_iso)
    server_updated_at: str | None = None
    version: int = 0
    sha256: str
    size_bytes: int
    filename_hint: str | None = None
    platform_source: str | None = None
    conflict: bool = False

    @field_validator("game_id")
    @classmethod
    def validate_game_id(cls, value: str) -> str:
        v = value.strip()
        if not v:
            raise ValueError("game_id cannot be empty")
        return v


class SaveListItem(BaseModel):
    game_id: str
    last_modified_utc: str
    server_updated_at: str | None = None
    version: int = 0
    sha256: str
    size_bytes: int
    filename_hint: str | None = None
    platform_source: str | None = None
    conflict: bool = False


class SaveListResponse(BaseModel):
    saves: list[SaveListItem]
