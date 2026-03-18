# 3DS Client (libctru 3DSX MVP)

Nintendo 3DS homebrew sync client using socket-based HTTP with libctru networking.

## Implemented features

1. Config parsing from `sdmc:/3ds/gba-sync/config.ini`
2. Local `.sav` scanning from configurable save directory
3. HTTP sync operations:
   - `GET /saves`
   - `GET /save/{game_id}`
   - `PUT /save/{game_id}`
4. Explicit overwrite sync actions:
   - Upload paths always force-overwrite server saves (`force=1`)
   - Download paths overwrite local save files
5. Atomic write safety for downloaded saves
6. Bottom-screen status output
7. ROM-header-based `game_id` derivation when ROM settings are configured

## Example config

```ini
[server]
url=http://192.168.1.50:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/saves
vc_save_dir=sdmc:/3ds/Checkpoint/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

Mode notes:

- `mode=normal`: sync from `save_dir`
- `mode=vc`: sync from `vc_save_dir` (useful with exported VC save flows)
