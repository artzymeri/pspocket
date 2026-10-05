# Run 096 · Route B on the iPhone: Assassin's Creed and GTA V prepared on the phone; SPU code and firmware without a Mac

2026-10-05 23:30 to 2026-10-06 00:20 · app 7b6e16e+ (the "Preparing game…" flow) by cable · iPhone 17 Pro Max, iOS 26.

## Preparing on the phone (the user tapped the games; prepare-progress.csv pulled after)

| Game | Files | All | Translate | Compile | Link | Sign | PPU rate |
|---|---|---|---|---|---|---|---|
| Assassin's Creed (BLES00158) | 368 | 3:37 | 3.2 s | 3:32 | 0.3 s | 0.5 s | 0.857 |
| GTA V (BLES01807) | 510 | 7:20 | 5.1 s | 7:11 | 0.5 s | 0.8 s | 1.053 |

PPU code only (the phone's SPU step came after this build). The time left on the phone:
- Assassin's Creed: +46% before it knew the game's size, +18% after translating, then within 11% from 20 s on.
- GTA V: 13-18% low most of the way. Its prior was Assassin's Creed's rate, measured on a cool phone; GTA V was
  prepared right after, warm ("fair"). The phone now records its heat with each preparation and scales the prior by
  the heat at the start (about 1.2 per level, D-154).

**GTA V then ran from the library the phone made:** "game library …/pspocket-games/BLES01807/game.dylib (BLES01807,
made by pspocket 0.1.0): 85376 PPU functions … signature registered, library validation passed", 29.5-29.9 fps in
its opening screens, as before. Assassin's Creed stayed on the boot overlay with an empty log (stuck before its first
line, before the disc is opened); the same library boots on the Mac and reaches its known stop (cellSyncMutexTryLock).
Not found yet: the next phone session attaches the debugger to it (`xcrun lldb`, `device process attach`).

## SPU code from images alone (Mac, D-155)

| SPU code | Misses | fps (median / slowest) |
|---|---|---|
| the Mac runs' seeded lift (lift-spu.sh) | 0 | 29.8 / 29.4 |
| images only, no discovery | 379 | 12.8 / 9 |
| images only, discovery, discovered code in the code words | 29 | 12.0 / 8.8 (jobs dropped and interpreted) |
| images only, discovery, discovered code out of the code words | 45 | 29.8 / 29.8 |

## Firmware from PS3UPDAT.PUP (Mac, D-156)

`pspocket-firmware inventory/firmware/PS3UPDAT.PUP DIR`: 4.92, 24 packages, 1,075 files, 186.0 MB in 1.4 s; the three
files the Mac's dev_flash copy had are identical; all 142 .sprx of sys/external decrypt with decrypt_self (libsre to
a 239,344-byte PPU PRX). Nothing of it kept outside the scratch folder.
