# Run 085 · Where the iPhone's CPU goes, and translated SPU calls that save less

2026-10-05 · cb10984+ (D-127) · iPhone (Time Profiler, whole device, 10 s) and M4 Mac (Time Profiler attached to
`pspocket-boot`, 20 s from 150 s of the walk of run 080: `--press cross@45 ... --press ls-up@185:30`).

The iPhone, in the first mission's escape, 15 minutes in, heat "serious", 9 to 17 fps:

| Thread | CPU in 10 s |
|---|---|
| Six SPU workers | 46.5 s (4.6 cores; 79% translated SPU code, 10% the SPURS kernel, 9% DMA) |
| RSX thread | 5.3 s (copies for draws, texture checks, FIFO) |
| PPU threads | 4.2 s (the game's audio thread 0.4 s, mostly waiting) |
| Everything in the app | 57.7 s: 5.8 of 6 cores |

Inside the translated SPU code (by source line, through the dSYM): register saves and reloads around calls 25%,
compiler-made moves and spills 18%, rotqby 7%, cflts 7% (per lane through double), shufb 7%, local store loads 7%.
Outside it: the channel lock's mutex 1.2 s, SPU_RdDec's clock and two 128-bit divisions 1.1 s, reservation write
notifications 0.85 s, the module tracker's GET observer 0.83 s.

The Mac, same scene window before and after D-127 (30 fps in both, no SPU code interpreted in either):

| | SPU workers | Translated SPU code | Saves (`psl_st`) | Reloads (`psl_ld`) |
|---|---|---|---|---|
| Before | 30.8 s | 14.8 s | 2.20 s | 2.74 s |
| D-127 | 29.1 s | 12.7 s (**-14%**) | 1.66 s | 2.24 s |

- `psl_cflts`, `psl_to128` and `psl_from128` (rotqby's 128-bit form) are gone from the profile.
- What is left of the saves and reloads is mostly each function's own: its entry loads every register it uses and
  its exits store every register it writes.
- The sound: every phone session plays the loading screens at full pace (938 blocks in 5 s) and the game at 0.5 to
  0.8 of it once it is in the mission, from the moment the SPU load rises; the sound thread waits on SPU work.
