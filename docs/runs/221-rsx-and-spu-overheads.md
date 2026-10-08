# Runs 221–233 · Where GTA V's frame goes on the Mac, and the overheads around it (D-240 to D-243)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt` (128 s;
the 60-90 s windows). Runs 221-229 were meant to be under load and ran without it (the spinner was started without its
arguments and stopped at once): those are the idle Mac. Runs 230-233 are under load (six threads spinning at the SPU
threads' QoS class). Logs in inventory/gta5 (kept out of the repository).

## The profile (run 221, `sample` 15 s at 70 s)

Busy time per thread, as a share of one core: the five main SPU threads 1.66 cores, the RSX thread 0.41, the audio SPU
thread 0.35, the PPU threads 0.33, two unnamed (Metal and the system) 0.29. The RSX thread, the busiest single thread
and the one every frame waits for, spent:

| | share of its busy time |
|---|---|
| a draw (do_draw), all of it | 78% |
| fragment programs (decoding programs found again at an address, their compile keys) | 19% |
| texture binds (of which the layout computed twice, with allocations: about 2%) | 9% |
| vertex data and constants copied into Metal buffers | 7.5% |
| the FIFO's IO address translation, out of line | 3.4% |

The main SPU threads, busy 35% of the time: their translated code, and around it the DMA (15%: the copies 4.5%, the
module tracker's GET observer 3.6%, the write notifications 2.5%), the SPURS kernel's selection (5%), and the clock
and the sleeps of idle polls (about 7%: sleep_for's clock request by Mach message, steady_clock reads).

## Fragment programs by content (D-240, runs 222-225)

| | before (222, 224) | after (223, 225) |
|---|---|---|
| the RSX thread, CPU s per 5 s | 1.71, 1.67 | 1.65, 1.64 |
| fps | 29.9, 29.7 | 29.9, 29.9 |

## The clock, the idle sleep, texture layouts, the FIFO translation (D-241, D-242, runs 226-229)

| | before (226, 228) | after (227, 229) |
|---|---|---|
| main SPU threads, CPU s per 5 s | 7.54, 6.65 | 7.23, 6.45 |
| the RSX thread | 1.68, 1.49 | 1.57, 1.47 |
| the process | 12.59, 11.26 | 11.99, 11.03 |
| fps | 29.7, 29.9 | 29.9, 29.9 |

The two "before" runs differ by 12%: only the pairs count, and both go the same way.

## Write notifications for watched lines only (D-243, runs 230-233, under load)

| | before (230, 232) | after (231, 233) |
|---|---|---|
| main SPU threads, CPU s per 5 s | 8.82, 8.77 | 8.53, 8.09 |
| the RSX thread | 2.12, 2.09 | 2.04, 1.95 |
| the process | 15.60, 15.44 | 15.07, 14.50 |
| fps | 28.7, 29.3 | 29.0, 28.8 |

Under load the process spends 25% more CPU than idle for the same frames (threads that wait for cores spin and yield
longer). Next: the SPURS line's waiters are all woken by every store to it (a mutex and a notify_all, about 3% of the
SPU threads' busy time); the GPU spends 24-26 ms of a frame in play (render passes: 84 a frame).
