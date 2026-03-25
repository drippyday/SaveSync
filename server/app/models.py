from __future__ import annotations

from datetime import datetime, timezone
from pydantic import BaseModel, ConfigDict, Field, field_validator


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


class SaveMeta(BaseModel):
    model_config = ConfigDict(extra="ignore")

    game_id: str
    last_modified_utc: str = Field(default_factory=utc_now_iso)
    server_updated_at: str | None = None
    version: int = 0
    sha256: str
    size_bytes: int
    rom_sha1: str | None = None
    filename_hint: str | None = None
    platform_source: str | None = None
    conflict: bool = False
    display_name: str | None = Field(default=None, max_length=128)

    @field_validator("game_id")
    @classmethod
    def validate_game_id(cls, value: str) -> str:
        v = value.strip()
        if not v:
            raise ValueError("game_id cannot be empty")
        return v

    @field_validator("rom_sha1")
    @classmethod
    def validate_rom_sha1(cls, value: str | None) -> str | None:
        if value is None:
            return None
        v = value.strip().lower()
        if not v:
            return None
        if len(v) != 40 or any(ch not in "0123456789abcdef" for ch in v):
            raise ValueError("rom_sha1 must be a 40-char hex SHA-1")
        return v


class SaveListItem(BaseModel):
    model_config = ConfigDict(extra="ignore")

    game_id: str
    last_modified_utc: str
    server_updated_at: str | None = None
    version: int = 0
    sha256: str
    size_bytes: int
    rom_sha1: str | None = None
    filename_hint: str | None = None
    platform_source: str | None = None
    conflict: bool = False
    display_name: str | None = None
    list_order: int = Field(
        default=0,
        description="Position in GET /saves (0 = first). Matches admin row order.",
    )


class HistoryEntry(BaseModel):
    filename: str
    size_bytes: int
    modified_utc: str
    display_name: str | None = None
    # ISO timestamp from index metadata when this backup was written (filename prefix; see storage._backup_existing)
    indexed_at_utc: str | None = None
    # Preformatted UTC 12h string for clients (same instant as indexed_at_utc or modified_utc)
    time_display: str | None = None
    # Pinned revisions are never deleted when trimming to HISTORY_MAX_VERSIONS_PER_GAME (unpinned pool is trimmed).
    keep: bool = False


class HistoryListResponse(BaseModel):
    entries: list[HistoryEntry]


class RestoreRequest(BaseModel):
    filename: str = Field(..., min_length=1, max_length=512)

    @field_validator("filename")
    @classmethod
    def filename_basename_only(cls, value: str) -> str:
        v = value.strip()
        if not v or "/" in v or "\\" in v or v.startswith(".") or ".." in v:
            raise ValueError("filename must be a plain basename")
        return v


class DisplayNamePatch(BaseModel):
    display_name: str | None = Field(None, max_length=128)


class RevisionLabelPatch(BaseModel):
    filename: str = Field(..., min_length=1, max_length=512)
    display_name: str | None = Field(None, max_length=128)

    @field_validator("filename")
    @classmethod
    def filename_basename_only(cls, value: str) -> str:
        v = value.strip()
        if not v or "/" in v or "\\" in v or v.startswith(".") or ".." in v:
            raise ValueError("filename must be a plain basename")
        return v


class RevisionKeepPatch(BaseModel):
    filename: str = Field(..., min_length=1, max_length=512)
    keep: bool = False

    @field_validator("filename")
    @classmethod
    def filename_basename_only(cls, value: str) -> str:
        v = value.strip()
        if not v or "/" in v or "\\" in v or v.startswith(".") or ".." in v:
            raise ValueError("filename must be a plain basename")
        return v


class SaveListResponse(BaseModel):
    saves: list[SaveListItem]
    total: int | None = Field(
        default=None,
        description="Total rows before limit/offset slicing; equals len(saves) when not paginating.",
    )


class AdminSettingsPatch(BaseModel):
    """Runtime settings the admin UI can change (in-memory until process restart for some env-backed defaults)."""

    history_max_versions_per_game: int | None = Field(default=None, ge=0, le=1_000_000)


class IndexRoutingPut(BaseModel):
    """Replace index routing maps (aliases, ROM SHA-1 → canonical, tombstones)."""

    aliases: dict[str, str] = Field(default_factory=dict)
    rom_sha1: dict[str, str] = Field(default_factory=dict)
    tombstones: dict[str, str] = Field(default_factory=dict)


class SaveOrderPut(BaseModel):
    """Full ordered list of ``game_id`` values (must match ``GET /saves`` exactly)."""

    game_ids: list[str] = Field(default_factory=list)
