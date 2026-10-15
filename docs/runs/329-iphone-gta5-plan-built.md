# Run 329 · GTA V on the iPhone 18 Pro Max with the steady-30 plan built

2026-10-15 14:11 to 14:37 · main at d8444c6 (the app built at cc681e1: D-952 to D-986 but EDGE in Metal compute) ·
iPhone 18 Pro Max, iOS 27, on USB to the Mac · installed with ideviceinstaller (devicectl's developer disk image still
fails for this phone), the game's log pulled over AFC and the device log streamed (the USB link dropped from 14:12 to
14:32: nothing of that stretch but what the game's log kept), the user playing (the city, driving).

## The session (the game's log from 23,382 frames since the boot to 30,691)

- Every native piece was in: "EDGE geometry ... as a derived program (8 of 17 listed functions native)", flashMP3,
  zlib inflate, RAGE's synthesizer, submixes and voices (D-965 to D-983).
- Busy windows (EDGE/GFX 7 s or more of SPU time in 5 s and 4.5 cores or more, as runs 327 and 328 counted them): 154
  at **24.7 fps** (median 24.3), against 21.4 in run 328 and 18.1 to 18.9 in run 327 (+15% and +31 to 36%), at a
  median of 2,969 draws a frame (3,000 to 3,300 in run 328's heaviest stretches; city driving 3,400 to 4,000 here at 17
  to 23 fps). Lighter windows (61): a median of **29.9 fps**. All 233 windows: median 26.5, 36% at 29 or more.
- Sound: the game's pace a median 1.03; 0.13% of the periods late in busy windows (3.0% in run 328, 5.5 to 15% in run
  327); the speaker dry now and then (one to six times in a few windows). The mixer (Secondary) 1.9 to 3.3 s of 5 s,
  no longer a whole core.
- Heat: serious for 165 of the 233 windows, fair 57, nominal 12. Power a median 3.1 W, 40% of the CPU on the
  performance cores; 4.9 to 5.2 cores busy in the city.
- Where the CPU goes in city driving: EDGE/GFX 9.2 to 10.4 s of SPU time in 5 s (three SPU threads at about 78%),
  dependency_spu 3.1 to 4.4, the mixer 2 to 3.3, the RSX thread 2.4 to 2.9, GraphicsMain about 2.2. EDGE a draw about
  29 µs of SPU time against 31 in run 327's budget: the native pieces' gain (-26 to -38% on the Mac, A/B) is mostly not
  seen here, with the phone hot and the city busier; an in-run A/B (PSPOCKET_SPU_NATIVE=ab in
  Documents/pspocket-env.txt) would say how much.
- The GPU: a median 9.3 ms a frame, 14.6 at most: two thirds of a 30 fps frame free.
- Present: drawable waits a median 2.9 ms in 5 s.
- No crash, no fatal; the only errors cellGcmSetTileInfo's compression modes 7 and 10 (as before). The session ended
  with the app sent to the background at 14:37:36 (a command buffer refused from the background, as expected).

## What came of it

- The plan's CPU work lifts busy scenes to some 25 fps and lighter ones to 30, and fixes the sound; city driving
  stays CPU-bound (five cores, serious heat) with EDGE the largest part. The GPU is two thirds idle: EDGE in Metal
  compute (D-987 on, in progress) moves the largest CPU item to it.
