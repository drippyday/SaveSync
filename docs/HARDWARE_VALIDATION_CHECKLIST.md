# Hardware Validation Checklist

Use this checklist for real-device validation on Switch + 3DS + Delta.

## Prerequisites

- [ ] Server is running and reachable from all devices
- [ ] API key configured and identical on every client
- [ ] Console config files point to the same server URL
- [ ] Delta bridge is configured with correct save folder

## Stage 1 - Server + Bridge sanity

- [ ] Run server health check (`GET /health`)
- [ ] Run bridge `--once` with one known `.sav`
- [ ] Confirm `GET /saves` shows uploaded save
- [ ] Remove local save and run bridge `--once`
- [ ] Confirm save is downloaded and hash matches original

## Stage 2 - Switch validation

- [ ] Install `gbasync.nro` and config
- [ ] Run app and confirm no config/network error
- [ ] Full sync: confirm screen — **A** continues, **B** returns to menu (**+** should not cancel confirm)
- [ ] After sync: **done** screen — **A** returns to menu, **+** exits app
- [ ] Save in Switch-side emulator, then run upload-only sync (`X`)
- [ ] Confirm server metadata hash/platform_source updates
- [ ] Pull same save on bridge/Delta and verify progress

## Stage 3 - 3DS validation

- [ ] Install `gbasync.3dsx` and config
- [ ] Run app and confirm no config/network error
- [ ] After sync: **done** screen — **A** returns to menu, **START** exits app
- [ ] Save in 3DS-side emulator, then run upload-only sync (`X`)
- [ ] Confirm server metadata hash/platform_source updates
- [ ] Pull same save on bridge/Delta and verify progress

## Stage 4 - Cross-device handoff tests

- [ ] Switch -> Delta handoff works (`X` on source, then pull on destination)
- [ ] Delta -> 3DS handoff works (bridge push, then `Y` on 3DS)
- [ ] 3DS -> Switch handoff works (`X` on 3DS, then `Y` on Switch)
- [ ] Repeat with 2+ games

## Stage 5 - Conflict behavior

- [ ] Modify same game on two devices while offline
- [ ] Sync device A, then sync device B
- [ ] Check `GET /conflicts` for expected flag
- [ ] Resolve via `POST /resolve/{game_id}` and re-validate final state

## Stage 6 - Regression checks

- [ ] Optional: remove a test `game_id` with `DELETE /save/{game_id}` and confirm it disappears from `GET /saves`
- [ ] Restart server and verify data persists
- [ ] Re-run bridge `--watch` for at least 5 minutes
- [ ] Reboot console and run sync again
- [ ] Confirm no save corruption after repeated sync cycles
