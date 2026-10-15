# Run 332 · GTA V on the iPhone 18 Pro Max: EDGE on the GPU after D-990

2026-10-15 21:30 to 21:37 · main af05874 (D-990: the jobs told one triangle, the driver's own steps left to the SPU,
notes once a kind) · the toggle on (Settings, D-996), the game prepared before (translator generation 5) · on USB until
21:37, heat nominal to fair, the user driving in the city. (An Assassin's Creed start just before froze at loading as in
run 331: D-998 on.)

## Measured (busy windows: 4.5 cores or more)

| | run 329 (no GPU path) | run 331 toggle on (before D-990) | this run |
|---|---|---|---|
| fps | 24.7 | 16 to 20 | **27.1** (24 to 29) |
| GPU a frame | 9 to 10 ms | 17.5 ms | 10.3 to 11.7 ms |
| warnings | | 97,679 | 189 |
| heat | serious | nominal | nominal / fair |

- ms of CPU a frame against run 329: EDGE/GFX SPU time 61 (76), dependency_spu 26 (34), GraphicsMain 23 (23), the mixer
  20 (20), the RSX thread 20 (22), the PPU's main thread 9 (12); 4.8 cores busy, 40% of the CPU on the performance
  cores, 3.2 W; sound pace 1.03.
- Jobs the GPU path leaves to the SPU (said once each): GTA V's triangle filter (job descriptor bytes 0x194, 0x1a4),
  fixed point 13.12 positions, a secondary attribute of id 9. EDGE's SPU time fell 20% here against 65% on the Mac's
  prologue walk: the city has more of those jobs.
- Not measured: the same route with the toggle off on a cool phone (the cable dropped at 21:37), and whether the
  shadows still flicker (the user's eyes).

## Next

Research on the last 10 to 15% to a steady 30 (two tracks: the SPU work left, and the critical path of the RSX thread,
GraphicsMain and the PPU on the phone's cores).
