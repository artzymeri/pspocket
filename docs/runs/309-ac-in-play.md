# Runs 309–311 · Background priority is no iPhone, and Assassin's Creed in play (D-276)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac. Logs in inventory/gta5 and inventory/BLES00158 (kept out
of the repository).

## GTA V on the efficiency cores (run 309)

The iPhone runs the game's SPU threads on its efficiency cores more than the Mac does, so run 309 tried
`taskpolicy -b` (background priority, which macOS keeps on the efficiency cores) as a stand-in for the phone: GTA V's
prologue walk at 6-11 fps from the first menu on, with only 0.5-0.6 cores busy. The timers and waits slow down at
that priority too, so the frame rate says nothing about the work. Not a proxy for the phone; dropped.

## Assassin's Creed, 600 s of its training walk (runs 310 and 311)

`games/ac1/walk-training.txt`, 600 s, prepared with the PPU continuations learned so far.
- **Run 310:** 196,608 PPU instructions interpreted (31 places where translated code returned into code the
  continuations did not have yet; written to ppu-landings.txt) and 2 RSX failures: two fragment programs reading WPOS
  in a multisampled target.
- **Run 311,** prepared again with the 38 continuations and with D-276 (WPOS is its pixel's on such a target):
  **0 PPU instructions interpreted, 0 RSX failures,** 31,257 flips.
- Frame rate (flips a second, 5 s windows): 18-19 in the boot's first minute, 60 through the menus and the
  cutscenes, 56-60 through most of the training, and 31-43 in its last minute (560-600 s), the heaviest stretch.
- There the CPU is the limit: 3.7-4.4 cores busy, the RSX thread 2.9-3.2 s in 5 s (60% of a core, the busiest
  thread), four SPURS threads 2.5-3.0 s each, the PPU's TaskThread2 1.9-2.4 s.

Next: where the RSX thread's time goes in that stretch (`sample`), then cut it.
