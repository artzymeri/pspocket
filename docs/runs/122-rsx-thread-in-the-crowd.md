# Runs 122–125 · The RSX thread in Assassin's Creed's crowd

2026-10-07 · pspocket-boot with the Metal backend · M4 Mac · Assassin's Creed (BLES00158), `games/ac1/walk-training.txt`
(600 to 620 s) to the training's "Gentle Push" lesson: a crowd of dozens of animated people, 1,700 to 2,200 draws a
frame. macOS `sample` on the running process for 10 to 15 s in the crowd; logs and profiles in inventory/BLES00158 and
the scratch folder (kept out of the repository).

## Where the RSX thread's time went (run 122, before)

In the crowd (run 121): 5.7 cores busy, the RSX thread 81% of one, the game at 25 to 29 fps against its 30, the GPU
22 ms a frame. The profile of the RSX thread (self time):

| | share |
|---|---|
| copying (memmove): the draw's vertex and constant uploads 11%, texture uploads 6.5% | 20% |
| decoding vertex programs again (the game uploads one before nearly every draw) | 7% |
| decoding fragment programs again (constants written in place) | 3.7% |
| the command reader (step_commands, dispatch, io_to_ea) | 19% |
| watching memory (WriteTracker::watch, mprotect) | 6% |
| allocations | 5% |

## The changes and their effect

| run | change | RSX thread, s per 5 s | µs per draw | fps | write faults a frame |
|---|---|---|---|---|---|
| 122 | (before) | 3.9–4.1 | 13.0 | 25–28.8 | (170–210) |
| 123 | D-210: programs found by their words | 3.4–3.8 | 11.6–12.2 | 29–30.2 | |
| 124 | D-211: interleaved vertex data copied once | 3.2–3.5 | 11.7 | 30–31.2 | 170–210 |
| 125 | D-211: fragment programs checked by bytes, unprotected | 3.0–3.4 | 11.1–11.6 | 30.4–31.8 | **0.1** |

The windows compared are the same stretch of the walk without the sampler. The decoders left the profile; the draw's
copying fell from 11% to 3.6% of the thread. What is left: the command reader (about 15%), texture uploads (6.7%),
memory watching for textures (about 7%), the byte compares of D-210/D-211 (4.8%).

## The gate

After D-210 and D-211 (787e5ee): the first run failed one check, 3 frame jumps (3184, 3228 and 3246 at 25.2 against a
threshold of 25), the third just after the scene cut the walk always has. The same third jump appeared once before on
2026-10-06 (5cdea63, 3250 at 25.0) and the next run of that code passed: the cut lands a frame earlier or later. The
game step again: **PASS** (inventory/gate/20261007-024742-787e5ee): 29.8 fps median, 29.6 in the slowest window, 2
frame jumps, 23.9 GPU ms per frame at worst, **write faults per frame 0.0 (40.6 before)**, 991 MB at peak, no new
warning shapes.
