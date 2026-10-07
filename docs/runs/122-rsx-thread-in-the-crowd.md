# Runs 122–133 · CPU time in Assassin's Creed's crowd: the RSX thread, the SPU tracker, the fiber switches, textures

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

## The SPU threads (runs 126–127): the module tracker, not changed

The five SPU threads of the crowd use about 3.5 cores; their profile puts the module tracker's GET observer (D-079)
at 11-12% of their busy time. A counting build (run 127's walk) found why: 1.6 to 3 million GETs a second at peak, 85%
of them 256 bytes or less, each testing about 12 runs and cutting or dropping 1.4, about 240 ns each. A free-slot list
(run 126) and run extents kept in the blocks (run 127) changed nothing measurable, and were reverted. GTA V has code
modules of 32 to 176 bytes, so small GETs cannot be skipped; the way on is in improve.md (a lazy ring of GETs).

## The fiber switches (runs 128–129, D-213)

Run 128 (a longer walk, `games/ac1/walk-training2.txt`, 690 s): 8.1 million fiber switches through the interpreter
(about 12,000 a second in the later scenes). The profile of the PPU task threads in the crowd: most of their time in
the C++ unwinder (dyld's image lookup per frame, the unwinder's lock contended by the two threads), from the exception
D-161 throws for each switch. D-213 goes back with _longjmp instead:

| | runs 125 and 127 | run 129 |
|---|---|---|
| the task threads, CPU s per 5 s | 5.45 | **1.89** |
| the whole process, CPU s per 5 s | 27.4–28.3 | **20.8** |
| fps | 29.8–31.2 | 33.7 |

The gate after D-213 (f810feb): **PASS**, 29.8 fps median and in the slowest window, 2 frame jumps, write faults 0.

## Textures over several targets (runs 130–132, D-214)

The RSX thread's profile after D-213 still had about 15% in texture decoding (memmove from guest memory, then watching
and unwatching the pages). New per-frame counts (run 130): 2 textures decoded a frame, both over a target drawn to
again. A one-off log of their shapes (run 131):

| texture | the target at its address |
|---|---|
| 4096x1024, row pitch 16 KB | a 1024x1024 depth buffer, the same pitch |
| 2560x720, row pitch 10 KB | a 1280x720 depth buffer, the same pitch |

The textures span several depth buffers side by side (the shadow cascades, by the look of it): only the one at the
texture's address was laid over (D-137); the others' part came from guest memory, which the GPU's drawing never
reaches, so it was stale. D-214 lays every GPU-newer target in the texture's rows over it, and copies nothing from
memory when they cover it whole (run 131 was the first, narrower try: the same address only).

| run | | RSX µs per draw | fps |
|---|---|---|---|
| 129 | (before) | 11.0 | 33.7 |
| 131 | covered: the target at the address only | 10.4 | 32.0 |
| 132 | every target in the rows | **9.2** | 32.7 |

Run 132's log also showed a compressed texture inside a large depth buffer's memory (another layout): such targets are
not laid over (as before), only those in the texture's own rows. The gate after D-214: **PASS** (29.8 fps, 2 frame
jumps, no new messages).

