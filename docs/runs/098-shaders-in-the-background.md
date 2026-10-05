# Run 098 · New shaders compile in the background (D-158): GTA V with an empty shader cache

2026-10-06 · pspocket-boot after D-158 · M4 Mac (busy: three agents building in parallel, load average up to 40) ·
GTA V (BLES01807), the gate's prologue walk (games/gta5/walk-prologue.txt), 128 s, Metal, a new empty
`--shader-cache` directory each run: the phone's first session with a game it has never seen.

| Run | 5 s windows (fps) | RSX thread compiling, burst window | Draws left out per frame, burst window | Programs and pipelines made |
|---|---|---|---|---|
| waiting compiles (as before) | 6.8, 14.2, 18.2, 18.8 lowest; 21 of 25 at 26-30 | 9.3 s | 0 | 728 |
| background, programs first, 2 threads | all 29.4-30.0 | 0.6 s | 287 | 657 |
| background, pipelines first, 4 threads | 23 of 25 at 29.2-30.2; 27.2 and 25.0 late, where little compiled (the busy Mac) | 0.5 s | 117 | 647 |

The burst is the walk's first scene change (flips 1200-1500): 562 programs and pipelines at once. Waiting, that
window lasted 9.3 s longer than its 300 frames; in the background the game kept its 30 fps and drew without the
draws whose shaders were still compiling. Programs first made every program of the scene before the first pipeline
(1 pipeline in that window); pipelines first finish draws as soon as their two programs are there.

The phone compiles several times slower than the M4 (D-114: run 082), so waiting would cost it tens of seconds in
the same stretch. The 4 ms budget per frame is the M4's; to be tuned on the phone.
