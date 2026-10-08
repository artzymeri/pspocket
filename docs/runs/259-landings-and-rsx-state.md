# Runs 259–268 · Fiber landings translated, and the RSX state games set (D-255 to D-267)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac, quiet (no simulator). Assassin's Creed (BLES00158) from
its prepared game library, `games/ac1/walk-training.txt` (150 s, `--rsx-recover`); GTA V (BLES01807) through the gate.
Logs in inventory/ (kept out of the repository).

## Assassin's Creed's landings, round by round

Each round prepared the game with the landings found so far (`pspocket-prepare --ppu-continuations`), then ran the
walk with `--ppu-landings`, which adds where the interpreter ran.

| Run | Continuations | PPU instructions interpreted | Where |
|---|---|---|---|
| 259 | 0 | 1,572,864 | 0xcf6d90 (the setjmp's return, 37,336 landings) |
| 260 | 1 | 1,572,864 | 0x00899adc: one frame up |
| 261-263 | 2-4 | 1.44 to 1.57 million | 0x00c265b0, 0x00c25a20: one more frame up each round |
| 264 | 5 | 1,572,864 | 0x00bf1b78, 0x00bf4978 |
| 265 | 6 | 1,638,400 | 0x00d24730 (5,772 landings: the two paths met) |
| 266, 267 | 7 | **0** | none; 0 new landings |
| 268 | 7 | **0** | the final build (resume runs the continuation first) |

Each round found only the next frame up, because only where the interpreter was entered was noted. Since then every
return the interpreter makes into code without a translation is noted too, so one session finds the whole chain
(tests/ppu jmp_wrap). The walk's fps does not move (median 59.8 in every run; the slowest window 15.8-18.4, the
loading stretch): the landings were 0.1% of a core. The point is the goal: no PPU code interpreted.

Run 268 also had the RSX changes: 0 failures, 0 draws split, the same frames.

## The RSX state census

Every NV4097 register the front end stores was matched against what the Metal backend reads, and each one it did not
read against RPCS3. 32 were stored without a use; 7 change what a game sees: the fog factor in FOGC (D-256),
two-sided lighting (D-257), the clip range and depth clamp (D-258), user clip planes (D-259), constants and vertex
bases written between the ranges of one draw (D-260), the depth bounds test (D-261). Then the rest of the list: point
sizes and sprites (D-262), polygon stipple (D-263), the alpha reference on float targets (D-264), the window offset
(D-265), an exported depth's clamp (D-266: Metal already clamps, nothing to change), the vertex output mask (D-267).
Each has a GPU test in tests/rsx (test_metal.mm).

What GTA V's walk uses of them:

- **The depth bounds test: 38 to 91 draws a frame in play** (its lights), until now drawn without it. One more copy of
  the depth buffer a frame (feedback copies 5 to 6) and one more render pass (84.1 to 85.1); GPU time per frame in
  play 15.1-17.0 ms against 15.9 before, within run-to-run noise.
- The vertex output mask: 19 vertex programs write outputs the mask leaves out. The first version of D-267 made a
  variant for each, 23 compiles in play in the gate (failed); none of those outputs is read by its fragment program,
  so variants are now made only for outputs the fragment program reads: none in the walk.
- 0 draws split (its constants change between draws only), no clip planes, no point sprites seen.

## The gate

`scripts/gate.sh` after D-267 (translation version 4: one walk first to make the shader cache again, which fails on
compiles in play as expected): tests and spu-check (150 programs, 0 differences) pass; GTA V's walk passes, median
30.0 fps, slowest window 28.2, 2 compiles in play, GPU 15.1 ms a frame at worst in play, 0 RSX failures.

Assassin's Creed (run 268, the final PPU build with D-255 to D-260): 0 RSX failures, the walk's fps as before
(median 59.8).
