# 3DS Client (libctru 3DSX MVP)

Nintendo 3DS homebrew sync client using socket-based HTTP with libctru networking.

## Implemented features

1. Config parsing from `sdmc:/3ds/gba-sync/config.ini`
2. Local `.sav` scanning from configurable save directory (`mode=normal` or `mode=vc`)
3. ROM-header-based **`game_id`** when `[rom]` paths are configured (else normalized stem)
4. HTTP sync operations:
   - `GET /saves`
   - `GET /save/{game_id}` (+ `/meta`)
   - `PUT /save/{game_id}` (`force=1` on uploads from this client)
5. **Auto (A):** **`.gbasync-baseline`** + SHA-256 policy (legacy **`.savesync-baseline`** still supported); first-run **SKIP** until X/Y seeds baseline; **Conflict** UI (X/Y/B)
6. **Upload** / **download** pickers; **START** / **R** / **X** or **Y** to run batch, **B** back
7. **Full-sync confirm** (A / B / START per on-screen copy)
8. **Post-sync:** **A** main menu, **START** exit app (skips second exit prompt)
9. Atomic writes; bottom-screen UI; resilient HTTP (chunked / encoding / JSON tolerance)

## Example config

```ini
[server]
url=http://10.0.0.151:8080
api_key=change-me

[sync]
mode=normal
save_dir=sdmc:/mGBA
vc_save_dir=sdmc:/3ds/Checkpoint/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

Mode notes:

- `mode=normal`: sync from `save_dir`
- `mode=vc`: sync from `vc_save_dir` (useful with exported VC save flows)
