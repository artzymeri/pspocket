# Run 100 · GTA V on the iPhone at 30 fps for two minutes, then the EDGE/GFX job chain stop

2026-10-06 morning · the iPhone build of 2e2d96e (D-178), installed by cable · iPhone 17 Pro Max · GTA V (BLES01807),
its library prepared on the phone earlier (85,376 PPU functions, 33 SPU programs) · no system software installed in this
copy of the app (only Documents/dev_flash with the SPU decoders the game loads itself), so no firmware modules and no
system library.

## Numbers (the game's own 5 s lines)

| | |
|---|---|
| frame rate | 29.5 in the first window, then 29.8 to 29.9 in every one, to 3,255 frames (about 110 s) |
| sound | 742 periods per 5 s, none skipped, none silent; the game's pace 0.99 to 1.00 |
| memory | 0.78 GB at the start, 1.03 GB, then 1.38 to 1.41 GB; 5.0 GB left |
| heat | fair throughout |
| CPU | 3.8 to 4.2 cores busy: the six SPU workers 1.5 to 3.5 s each per 5 s, the RSX thread 2.0 to 2.3 s, the main thread 0.8 s |
| GPU | 17 to 18 ms per frame, 820 to 870 draws and 76 to 79 render passes a frame |

The frame rate holds 30 where earlier phone runs (087 to 089) dropped to 10-20 in the busy scenes: the walk here did
not reach those scenes, so this says nothing about them yet. Memory stays under 1.5 GB (D-148's spike fix holds on the
phone).

## The stop

After about 110 s the EDGE/GFX job chain (workload 11, SPURS 0x10015000, SPU 3) fetched a job binary at EA 0x10: the
job descriptor at 0x1024d400 held RSX command words (00041820 00000010, 00041808 00000005 = SET_BEGIN_END triangles,
400c1824 ...), not a descriptor. The same overwrite as D-130, D-131, D-138 and run 095 (docs/improve.md, stability):
something wrote over a job descriptor before the chain read it. This stop came through the job binary's fetch, a path
that stopped without D-138's report of who wrote over the descriptor; every job chain stop now makes that report
first.

## Next

The Mac walk again, repeatedly, under the load of four builds, to catch the stop with the writer named; then the
writer's output address against the descriptor ring (docs/improve.md).
