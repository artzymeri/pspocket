# Run 090 · The first gated walks (D-148, D-149), the Mac's two GPU speeds, and Assassin's Creed's first failure

2026-10-05 18:30-19:40 · working tree on e481091 (D-148 and D-149 before their commits) · M4 Mac.

**GTA V's prologue walk through the gate** (`scripts/gate.sh game`, games/gta5/walk-prologue.txt, 128 s):

| Run | fps (5 s windows) | Frame jumps | Passes a frame (play) | Feedback copies a frame | GPU memory | Process memory | GPU ms a frame |
|---|---|---|---|---|---|---|---|
| walk g of run 088 (before D-148) | 29 to 30 | 2 | 72 to 87 (target copies 8 to 16) | (part of the target copies) | - | - | 7.95 to 8.85 |
| gate run 1 (copies reused in place) | 29.2 to 30 | 2 | 71 to 84 | 5 | 224 MB | 1.6 GB | 12.5 to 23.7 |
| gate run 2 (a new copy per write, as D-148) | 29.2 to 30 | 2 | 71 to 84 | 5 | 231 MB | 1.6 GB | 14.6 to 24.2 |
| without frame capture | 29.8 to 30 | - | 71 to 84 | 5 | - | - | 19.9 to 24.6 |

The GPU time looked like a regression, but run 088's own walks show the same two levels before any of this work:
walks a, b and d reach 22 to 25 ms, walk f goes from 8.5 to 22 ms halfway through, only walk g stayed at 8.5. The Mac's
GPU time per frame flips between the two (its own clocking, most likely) at 30 fps either way, so the gate keeps it as a
loose ceiling (30 ms) and checks render passes a frame instead (D-149). The memory the spike of run 089 came from is
gone from the walk: GPU memory flat at 220 to 230 MB.

**spu-check** over GTA V's dumped SPU programs (2 trials, 40 entries each): 22 programs in 10 minutes, no difference;
about 27 s a program, two hours for all of them, so it is `scripts/gate.sh spu`, run before the gate for SPU
translation changes (D-149).

**Compile cost of the lifters' C** (one M4 core, the build's flags; for route B's stage 2, D-147): PPU units of 1.9 MB
take 1.4 to 1.6 s and 3.3 MB 2.5 to 3.4 s at -O1 or -O2 (-O0 is no faster and makes objects six times larger): GTA V's
512 units about 14 CPU-minutes. SPU programs about 3.5 s per MB (8 MB: 28 to 32 s): 105 MB about 6.5 CPU-minutes.

**Assassin's Creed (BLES00158)**, first look on the Mac (interpreted, 90 s, the disc image read in place): SDK 1.92,
140 imports from 10 libraries, 100 implemented. The main thread spins on `cellSyncMutexTryLock` (not implemented: it
answers an error the game takes for "busy") 24.3 million times and never allocates the RSX; the phone's log of the same
game shows the same loop. Missing besides cellSync: cellSysutil (6), sys_fs asynchronous and stream reads (7), cellPad
(3), cellGcmSys (7), cellSpurs (3: CreateTaskset, AddUrgentCommand, SendSignal), sysPrxForUser (11: spinlocks, mmapper,
process_is_stack, spu_image_close, exitspawn). docs/compatibility.md has it as Broken.
