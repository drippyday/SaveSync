# Switch Client (libnx NRO MVP)

Nintendo Switch homebrew sync client using libnx and socket-based HTTP.

## Implemented features

1. Config parsing from `sdmc:/switch/gba-sync/config.ini`
2. Local `.sav` scan from `sdmc:/roms/gba/saves` (configurable)
3. Optional **ROM-header** `game_id` when `[rom]` paths are set (else normalized stem)
4. HTTP sync operations:
   - `GET /saves`
   - `GET /save/{game_id}` (+ `/meta`)
   - `PUT /save/{game_id}` (`force=1` on uploads from this client)
5. **Auto (A):** baseline file **`.savesync-baseline`** + SHA-256 (not SD mtime policy); first-run **SKIP** until X/Y seeds baseline; **Conflict** UI (X/Y/B)
6. **Upload (X)** / **download (Y)** pickers with checklist; **+** runs, **B** back
7. **Full-sync confirm:** **A** continue, **B** back (**+** intentionally not cancel on that screen)
8. **Post-sync:** **A** main menu, **+** exit app
9. Atomic write for downloaded saves; resilient HTTP parsing (chunked bodies, `Accept-Encoding: identity`, etc.)

## Example config

```ini
[server]
url=http://192.168.1.50:8080
api_key=change-me

[sync]
save_dir=sdmc:/roms/gba/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```
