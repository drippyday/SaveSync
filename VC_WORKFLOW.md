# 3DS VC Inject Workflow Guide

This guide explains how to use SaveSync with **GBA VC inject titles** on 3DS using a save-manager export/import flow.

Because VC saves are stored in title save archives, SaveSync does not directly patch VC save archives yet.  
Use this workflow to move saves in and out safely.

## What you need

- SaveSync server running
- SaveSync 3DS client installed
- Save manager on 3DS (for example, Checkpoint)
- Your VC inject title installed and playable

## High-level cycle

1. Export VC save from title archive to SD card
2. Sync exported save with SaveSync (`mode=vc`)
3. Import updated save back into VC title archive

## 1) Configure SaveSync 3DS for VC mode

In `sdmc:/3ds/gba-sync/config.ini`:

```ini
[server]
url=http://YOUR_SERVER_IP:8080
api_key=change-me

[sync]
mode=vc
save_dir=sdmc:/saves
vc_save_dir=sdmc:/3ds/Checkpoint/saves

[rom]
rom_dir=sdmc:/roms/gba
rom_extension=.gba
```

Notes:

- `mode=vc` tells SaveSync to read/write `vc_save_dir`.
- Keep `rom_dir` configured if you want better ROM-header-based game ID matching.

## 2) Export save from VC title

Using your save manager:

1. Launch save manager.
2. Select your VC GBA title.
3. Run **Backup/Export**.
4. Confirm backup appears under your save-manager export folder.

Typical path pattern (Checkpoint-style):

- `sdmc:/3ds/Checkpoint/saves/<Title Name>/<Backup Name>/...`

## 3) Make exported save visible to SaveSync

SaveSync expects `.sav` files inside `vc_save_dir`.

### Option A (recommended): point `vc_save_dir` to exact export folder

Set `vc_save_dir` to the specific backup folder containing your `.sav`, for example:

- `sdmc:/3ds/Checkpoint/saves/My GBA VC/Backup1`

### Option B: copy `.sav` into a stable folder

Create a stable folder and use it as `vc_save_dir`, then copy exported `.sav` there each cycle.

## 4) Sync with SaveSync

1. Launch `gbasync` on 3DS.
2. Confirm status lines (for example `UPLOADED`, `DOWNLOADED`, `OK`).
3. Press `START` to exit.

If needed, sync on another device (Switch/Delta bridge), then run 3DS sync again to pull latest changes.

## 5) Import save back into VC title

Using your save manager:

1. Ensure the target backup folder contains the updated `.sav`.
2. Select the VC title.
3. Run **Restore/Import** from that backup.
4. Launch the VC title and verify progress.

## Recommended naming strategy

- Keep save file stems consistent across devices.
- Use matching ROM in `rom_dir` so SaveSync can derive consistent IDs from ROM header.
- Avoid renaming save files mid-workflow.

## Troubleshooting

### SaveSync says no saves found

- Confirm `mode=vc`.
- Confirm `vc_save_dir` points to a directory containing `.sav` files.
- Confirm file extension is `.sav`.

### VC title did not load updated progress

- Ensure you restored/imported the correct backup slot.
- Ensure updated `.sav` is in the exact folder your save manager restores from.
- Re-export once, compare file size/hash, then re-import.

### Wrong game got synced

- Ensure unique save names per game.
- Configure `[rom]` section for ROM-header-based matching.

### Conflicts appeared

- Check server `GET /conflicts`.
- Resolve with `POST /resolve/{game_id}` after deciding which save is canonical.

## Safety best practices

- Keep at least one known-good backup before import.
- Test with one game first.
- Do not overwrite multiple backup slots at once until workflow is stable.
