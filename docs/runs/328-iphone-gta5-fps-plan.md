# Run 328 · GTA V on the iPhone 18 Pro Max with the first steady-30 work

2026-10-14 19:11 to 19:37 · main at f73d3a4 plus D-952 and D-970 to D-973 (uncommitted at the session) · iPhone 18 Pro
Max, iOS 27.0.1, on USB to the Mac (charging) · installed with ideviceinstaller, the game's log pulled over AFC and the
device log streamed, the user playing (the city by day, driving).

## Before the game ran

- The app prepared GTA V again (549 files, 372 s, compile 365 s): the runtime built for the A12 (D-952) is another
  runtime to the game library.
- The session's first lines: "cores: 2 performance, 4 efficiency; heat nominal; Low Power Mode off", "vblanks on the
  screen's refreshes; frames held 1/30 s at 30 fps" (D-971).

## The session (19:18 to 19:37, 26,083 frames)

- Busy windows (EDGE/GFX 7 s or more of SPU time in 5 s, 4.5 cores or more, as run 327's budget counted them): 174 at
  21.4 fps, against 18.1 to 18.9 in run 327's sessions (+15 to 18%); the sound late in 3.0% of their periods (5.5 to
  15% before), never dry. The heaviest stretches 15 to 20 fps (3,000 to 3,300 draws a frame, 5.3 to 5.5 cores),
  lighter ones 25 to 30. GPU 8 to 13 ms a frame. The heat started nominal, fair at 19:18:58, serious at 19:24:53 (the
  phone charging); run 327 was serious from its start, so part of the difference may be the heat.
- Power (D-970): a median 3.2 W (5.7 at most); 38% of the CPU time on the performance cores (34 to 81%: most of the
  load runs on the efficiency cores in busy scenes).
- Present (D-971): frames held 1/30 s while the game flipped every 33 to 60 ms; the RSX thread waited for drawables a
  median 2.4 ms in 5 s (one window of 2.2 s, the app coming back from the background): the drawable need not leave the
  RSX thread.
- In-session SPU translation at "serious" heat (D-970): 15 programs and modules translated while the game ran, each 0.3
  to 20 s after it was first met (most within 3 s), instead of the interpreter for the rest of the session.
- No crash, no memory warning, no black frames. The user quit the app at 19:37.

## What came of it

- D-952, D-970 to D-973 measured on the phone (above); D-973's switch to be measured with PSPOCKET_SPU_AB=30 and
  PSPOCKET_SPU_QOS_SERIAL=interactive (set in Documents/pspocket-env.txt for the next session, the build installed at
  19:38).
