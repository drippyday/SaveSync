# Delta Bridge

Desktop sync agent that bridges Delta save files and the SaveSync server.

## Modes

- `--once`: one pull/push pass then exit
- `--watch`: filesystem watch + periodic polling
- `--dry-run`: print actions without writing/uploading

## Setup

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp config.example.json config.json
python bridge.py --config config.json --once
```

## Config fields

- `server_url`: SaveSync server base URL
- `api_key`: server API key
- `delta_save_dir`: local Delta save folder to monitor
- `poll_seconds`: periodic poll interval
- `rom_dirs`: optional list of ROM directories for ROM-header-based `game_id`
- `rom_map_path`: optional JSON mapping save stem -> ROM path
- `rom_extensions`: optional ROM extensions used in `rom_dirs` matching

## Notes

- Game ID resolution order:
  1. If matching ROM is found, derive from GBA ROM header (`title + game code`)
  2. Fallback to normalized `.sav` filename stem
- ROM matching sources:
  - `rom_map_path` JSON mapping save stem to ROM path
  - `rom_dirs` + `rom_extensions` stem matching
