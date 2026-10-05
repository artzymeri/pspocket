# Run 082 · The shader cache in the first mission

2026-10-05 · 7ac7e8c+ (D-114) · M4 Mac · `pspocket-boot ... --timeout 300 --frames DIR --shader-cache
inventory/gta5/shaders`, the presses of run 080 (walk, phone, charge, shooting), the per-frame report every 300 flips.

| | Programs compiled in play | Pipelines | RSX thread compiling | fps at the mission's start |
|---|---|---|---|---|
| No cache (recording it) | 558 | 497 | 6.1 s at the start, 2.3 s in the vault scene, about 13.6 s in all | 12 to 24 |
| With the recording | 0 | a few (about 0.1 ms each) | under 1 ms | 28 to 30 |

- With the cache, the 558 programs and 495 pipelines were made on three background threads in 0.4 s while the game
  loaded (the system's Metal shader cache on the Mac helps; a phone's first time is slower, still in the background).
- The recording: 11 MB of MSL and pipeline lines in inventory/gta5/shaders (game data: never committed).
- PSPOCKET_RSX_CHECK_SURFACES over 170 s of the mission: no texture sampled a surface whose memory changed under it.
