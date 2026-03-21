"""Tests for server_delta game_id assignment when hacks share retail GBA headers."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_BRIDGE = Path(__file__).resolve().parent
if str(_BRIDGE) not in sys.path:
    sys.path.insert(0, str(_BRIDGE))

from delta_folder_server_sync import (  # noqa: E402
    _dedupe_plan_server_targets,
    _game_id_plan_for_server_delta,
)


def test_shared_header_assigns_retail_only_not_hack() -> None:
    logs: list[str] = []

    def log(msg: str) -> None:
        logs.append(msg)

    remote = {"pokemon-fire-bpre": {"sha256": "x"}}
    colliding = {"pokemon-fire-bpre"}
    rom_to_gid = {
        "aaa111": "pokemon-fire-bpre",
        "bbb222": "pokemon-fire-bpre",
    }
    delta_by_rom = {
        "aaa111": {
            "identifier": "HID_RETAIL",
            "name": "Pokémon: Fire Red Version",
            "save_json": None,
            "save_blob": None,
        },
        "bbb222": {
            "identifier": "HID_HACK",
            "name": "RedRocket",
            "save_json": None,
            "save_blob": None,
        },
    }

    plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, log)

    assert plan["HID_RETAIL"] == "pokemon-fire-bpre"
    assert plan["HID_HACK"] is None
    assert any(
        "RedRocket" in m
        and ("shares GBA header" in m or "not retail-shaped" in m or "ambiguous header" in m)
        for m in logs
    )


def test_bpre_collision_retail_fire_red_explicit_when_nk_equals_header() -> None:
    """Retail Fire Red must map to pokemon-fire-bpre even when that equals the ROM header slug."""

    def log(msg: str) -> None:
        pass

    remote = {
        "pokemon-fire-bpre": {"sha256": "x"},
        "unbound": {"sha256": "y"},
        "redrocket": {"sha256": "z"},
    }
    colliding = {"pokemon-fire-bpre"}
    rom_to_gid = {
        "fr111": "pokemon-fire-bpre",
        "rr222": "pokemon-fire-bpre",
        "ub333": "pokemon-fire-bpre",
    }
    delta_by_rom = {
        "fr111": {"identifier": "HID_FR", "name": "Pokémon: Fire Red Version"},
        "rr222": {"identifier": "HID_RR", "name": "RedRocket"},
        "ub333": {"identifier": "HID_UB", "name": "Unbound"},
    }
    plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, log)
    assert plan["HID_FR"] == "pokemon-fire-bpre"
    assert plan["HID_UB"] == "unbound"
    assert plan["HID_RR"] == "redrocket"


def test_emerald_and_pokescape_share_bpee_header_maps_both() -> None:
    """Retail Emerald title must map to pokemon-emer-bpee even though slug ≠ server key."""

    def log(msg: str) -> None:
        pass

    remote = {
        "pokemon-emer-bpee": {"sha256": "x"},
        "pokescape-2.0.0": {"sha256": "y"},
    }
    colliding = {"pokemon-emer-bpee"}
    rom_to_gid = {
        "aaa111": "pokemon-emer-bpee",
        "bbb222": "pokemon-emer-bpee",
    }
    delta_by_rom = {
        "aaa111": {"identifier": "HID_EM", "name": "Pokémon: Emerald Version"},
        "bbb222": {"identifier": "HID_PS", "name": "PokeScape 2.0.0"},
    }
    plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, log)
    assert plan["HID_EM"] == "pokemon-emer-bpee"
    assert plan["HID_PS"] == "pokescape-2.0.0"


def test_bpee_duplicate_payload_hash_skips_hack_mapping() -> None:
    logs: list[str] = []

    remote = {
        "pokemon-emer-bpee": {"sha256": "same"},
        "pokescape-2.0.0": {"sha256": "same"},
    }
    colliding = {"pokemon-emer-bpee"}
    rom_to_gid = {
        "aaa111": "pokemon-emer-bpee",
        "bbb222": "pokemon-emer-bpee",
    }
    delta_by_rom = {
        "aaa111": {"identifier": "HID_EM", "name": "Pokémon: Emerald Version"},
        "bbb222": {"identifier": "HID_PS", "name": "PokeScape 2.0.0"},
    }

    plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, logs.append)

    assert plan["HID_EM"] == "pokemon-emer-bpee"
    assert plan["HID_PS"] is None
    assert any("same sha256 as /save/pokemon-emer-bpee" in m for m in logs)


def test_solo_retail_fire_red_still_maps_header() -> None:
    def log(msg: str) -> None:
        pass

    remote = {"pokemon-fire-bpre": {"sha256": "x"}}
    colliding = {"pokemon-fire-bpre"}
    rom_to_gid = {"aaa111": "pokemon-fire-bpre"}
    delta_by_rom = {
        "aaa111": {
            "identifier": "HID_RETAIL",
            "name": "Pokémon: Fire Red Version",
        },
    }
    plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, log)
    assert plan["HID_RETAIL"] == "pokemon-fire-bpre"


def test_redrocket_not_poisoned_with_fire_red_after_unbound_maps_explicitly() -> None:
    """Regression: sole unmapped BPRE hack must not receive pokemon-fire-bpre bytes (crashes Delta)."""

    def log(msg: str) -> None:
        pass

    remote = {"pokemon-fire-bpre": {"sha256": "x"}, "unbound": {"sha256": "y"}}
    colliding = {"pokemon-fire-bpre"}
    rom_to_gid = {
        "aaa111": "pokemon-fire-bpre",
        "bbb222": "pokemon-fire-bpre",
    }
    delta_by_rom = {
        "aaa111": {
            "identifier": "HID_UNBOUND",
            "name": "Unbound",
        },
        "bbb222": {
            "identifier": "HID_ROCKET",
            "name": "RedRocket",
        },
    }

    plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, log)

    assert plan["HID_UNBOUND"] == "unbound"
    assert plan["HID_ROCKET"] is None


def test_hack_with_dedicated_server_row_gets_own_id() -> None:
    def log(msg: str) -> None:
        pass

    remote = {"pokemon-fire-bpre": {"sha256": "x"}, "redrocket": {"sha256": "y"}}
    colliding = {"pokemon-fire-bpre"}
    rom_to_gid = {
        "aaa111": "pokemon-fire-bpre",
        "bbb222": "pokemon-fire-bpre",
    }
    delta_by_rom = {
        "aaa111": {
            "identifier": "HID_RETAIL",
            "name": "Pokémon: Fire Red Version",
        },
        "bbb222": {
            "identifier": "HID_HACK",
            "name": "RedRocket",
        },
    }

    plan = _game_id_plan_for_server_delta(delta_by_rom, rom_to_gid, remote, colliding, log)

    assert plan["HID_RETAIL"] == "pokemon-fire-bpre"
    assert plan["HID_HACK"] == "redrocket"


def test_dedupe_duplicate_server_id_keeps_explicit_name_match() -> None:
    plan = {"h1": "redrocket", "h2": "redrocket", "h3": "redrocket"}
    delta_by_rom = {
        "a": {"identifier": "h1", "name": "RedRocket"},
        "b": {"identifier": "h2", "name": "Unbound"},
        "c": {"identifier": "h3", "name": "PokeScape 2.0.0"},
    }
    rom_to_gid = {
        "a": "pokemon-fire-bpre",
        "b": "pokemon-fire-bpre",
        "c": "pokemon-fire-bpre",
    }
    logs: list[str] = []
    remote_meta = {
        "redrocket": {},
        "unbound": {},
        "pokescape-2.0.0": {},
    }
    _dedupe_plan_server_targets(plan, delta_by_rom, rom_to_gid, remote_meta, logs.append)

    assert plan["h1"] == "redrocket"
    assert plan["h2"] is None
    assert plan["h3"] is None
    assert any("duplicate mapping blocked" in m for m in logs)


def test_dedupe_fire_red_header_keeps_retail() -> None:
    plan = {"retail": "pokemon-fire-bpre", "hack": "pokemon-fire-bpre"}
    delta_by_rom = {
        "r": {"identifier": "retail", "name": "Pokémon: Fire Red Version"},
        "h": {"identifier": "hack", "name": "Unbound"},
    }
    rom_to_gid = {"r": "pokemon-fire-bpre", "h": "pokemon-fire-bpre"}
    _dedupe_plan_server_targets(
        plan, delta_by_rom, rom_to_gid, {"pokemon-fire-bpre": {}}, lambda _m: None
    )
    assert plan["retail"] == "pokemon-fire-bpre"
    assert plan["hack"] is None
