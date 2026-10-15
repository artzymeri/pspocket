# Run 331 · GTA V on the iPhone 18 Pro Max: EDGE on the GPU switched on from Settings

2026-10-15 17:14 to 18:00 · main 7b29e43 (D-987 to D-989 EDGE on the GPU, D-996 the Settings toggle, D-997 the sound
following the output, translator generation 5: the phone prepared GTA V again) · on USB, the user driving in the city.

## Toggle off (17:14 to 17:31, the game log game-171431)

- Busy windows 21.8 fps (run 329: 24.7), but the phone hot the whole time (serious in 179 of 183 busy windows, 2.4 W
  against 3.1 W in run 329): every workload slower alike, the mixer +30% a second though nothing changed in it. The
  preparation's five minutes at full power just before. Not a measure of the code.
- The first two tries "with the toggle on" were not: the setting is read when a game boots, and the log had no
  "(from Settings)" line (the app's preferences said edgeGPU false). Fully closing the app before switching matters.

## Toggle on (17:58, the game log game-175827)

- "PSPOCKET_EDGE_GPU=1 (from Settings)", "EDGE geometry on the GPU (PSPOCKET_EDGE_GPU=1): on". Heat nominal.
- Worse: 16 to 20 fps, a flip every 80 to 85 ms, only 3.0 to 3.5 cores busy; EDGE/GFX 10.3 to 11.9 s of SPU time in
  5 s while its three SPU threads used about 2.45 s of CPU each: the jobs blocked, not computing (a guess to verify:
  the culling's stand-in keeps every index, so the allocations are the full count and the 3 MB rings fill in the
  city, the jobs waiting on the RSX's label). GPU 17.5 ms a frame (9 to 10 off), no wait for it.
- 97,679 warnings "EDGE on the GPU: N indices to cull, M configured" (fewer indices than configured, about 3% of the
  jobs, never seen in the Mac's prologue walk; each distinct message logged).
- The user: shadows flickering on and off. The toggle went off again.

## What came of it

The GPU path is not for the phone yet; back to the agent with both logs (D-987 to D-989 follow-up). Lessons: the
Mac's prologue walk is too light to judge EDGE's rings; a phone run with the toggle needs the app fully closed first.
