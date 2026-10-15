# Run 330 · GTA V on the iPhone 18 Pro Max: the native SPU pieces on against off in one session

2026-10-15 15:18 to 15:27 · the app of run 329 (main cc681e1) · iPhone 18 Pro Max on USB, heat nominal to fair ·
Documents/pspocket-env.txt: PSPOCKET_SPU_AB=10, PSPOCKET_SPU_NATIVE=ab, PSPOCKET_SPU_NATIVE_TIME=1 (D-965: the native
replacements in half A only, 10 s halves) · the user driving in the city · 65 windows of 5 s, 28.9 fps on average.

## Measured

Each SPURS line gives a workload's time in the window and its time in half B; over the session the halves had the
same wall time and nearly the same frames (the game near 30 fps), so the sums compare directly. Workloads without a
native piece are the control.

| Workload | half A (native) | half B (lifted) | A/B |
|---|---|---|---|
| EDGE/GFX | 166.8 s | 208.5 s | **0.80** |
| zlibInflater | 6.7 s | 20.0 s | **0.34** |
| dependency_spu (none native) | 82.7 s | 81.1 s | 1.02 |
| GraphicsMain (none native) | 76.2 s | 78.4 s | 0.97 |
| AudioEngine (none native) | 26.6 s | 26.5 s | 1.01 |

- EDGE's native pieces (D-968) take 20% off EDGE/GFX on the phone (20 to 38% in the Mac's A/B walks, D-969); zlib
  inflate (D-975) two thirds off. The mixer (Secondary, SPURS 0x10016000) is not read here: its half-B times summed to
  more than one SPU's wall time, so its line's split is not usable as it stands.
- A first reading by the window's label (the half at the window's end) showed no difference: 5 s windows against 10 s
  halves mix both halves in most windows. Read the per-half times, not the label.
- PSPOCKET_SPU_NATIVE_TIME's per-call times are reported by pspocket-boot only; the app does not log them.
- The interpreter ran 20,000 to 53,000 SPU instructions in 5 s (entries the translation lacks, translated during the
  session), as in runs 328 and 329.
- The switch file was emptied after the session.
