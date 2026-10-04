# Run 026 · GTA V boot, round 24: frames

2026-10-04 · uncommitted tree on 437b72e · M4 Mac · `scripts/boot.sh` (inventory/boot-024.log).

| Step | Result |
|---|---|
| Job chains | All of them run as the game kicks them; no job chain error |
| Startup | The game looks for its startup and front-end data (startup.meta, frontend_menus.xml and others) and checks its game data |
| RSX | 2,008,834 command words, 1,721 flips: two draws and one clear per frame, as a loading screen would (null backend: nothing is shown) |
| Audio | The audio mix thread runs on its notify queue |
| Gaps | `sceNpManagerRegisterCallback`, `sceNpManagerUnregisterCallback` |
| End | 60 s timeout, no crash. The main thread waits on a semaphore from second 4; the RSX waits on a semaphore acquire (label 0x470 should become 1) |
