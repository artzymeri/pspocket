# Runs 304–308 · The SPU runtime's share, and render pass traffic (D-273)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt`.
Logs in inventory/gta5 (kept out of the repository).

## Threads (run 304, `sample` at 70 s)

CPU per 5 s in play: the SPURS threads about 6.5 s of 9 (the second instance's single SPU, "Secondary", the busiest
thread at 1.5-1.8 s), the RSX thread 0.8-1.1 s (20% of a core, from 42% before D-240), the PPU main thread 0.5-0.7 s.

Of the SPURS threads' busy samples, 68% are translated SPU code. The rest, the runtime: DMA copies 3.5%,
condition-variable broadcasts 2.7% (the kernel waking PPU threads on event flags, queues and semaphores the SPUs
set), clock reads about 4% together (the DMA write log's timestamp, the decrementer, timed waits), channel writes 1.9%,
the SPURS kernel's selection 1.4%, module identification and release 2.4%. No single item worth more than a few
percent. The Secondary SPU's work is spread over four programs.

## Render pass traffic (runs 305-308)

The status line now counts what render passes load from memory and store to it (D-273): about 188 MB loaded and
209 MB stored a frame in 85 passes, some 12 GB/s at 30 fps; passes that only clear: 3 a frame, 7 MB.

A traced frame (run 291) shows why: GTA V switches between its four G-buffer targets with depth and the first of them
alone (or depth alone) several times a frame, and each switch ended the pass. Keeping such draws in the open pass
(run 307): 85 to 78 passes, 129 MB loaded and 150 MB stored a frame (-30%), the frames right. In-run A/B (run 308):
GPU time a frame 13.54 to 14.40, 13.73 to 13.79, 14.29 to 15.33 ms with it: worse on the M4, whose bandwidth is
plentiful. Not kept; to try on the iPhone when it can be measured.

## The gate after D-273

Run 1 (20261008-173545): 9 compiles in play and a slowest window of 20.4 fps: the walk reached shaders the cache,
emptied by translation version 6 two runs before, had not recorded. The next gate (in scripts/ios.sh testflight)
passed.
