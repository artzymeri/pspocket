# Run 041 · GTA V boot, round 39: past calibration, "Loading Story Mode"

2026-10-04 · e935dfa · M4 Mac · `pspocket-boot ... --timeout 300 --flash inventory/firmware/dev_flash --frames ...
--frame-every 60 --press cross@45+10` (inventory/boot-040.log).

| Step | Result |
|---|---|
| Trophies | The trophy set NPWR03885_00 registers and installs (D-065): 50 trophies, 11 hidden; the status steps take 4.9 s |
| Input | Cross pressed at 45 s and every 10 s after (D-069) confirms the Display Calibration screen |
| Frames | **GTA V's "Loading Story Mode" screens**: the loading artwork changes over the run, with tips and the spinner |
| RSX | 8,203 flips, no failed draws |
| Gaps | `_cellSpursSemaphoreInitialize` (taskset "Physics", count 4) |
| End | The 300 s timeout, no crash. The main thread has waited 167 s in `cellSpursQueuePopBody` for the Physics tasks, which wait on the semaphore that was never set up |

Next: SPURS semaphores (RPCS3 has none: libsre is the reference).

**Where the time goes while loading** (macOS `sample`, 10 s at 90 s into a run of the same build): of about 36 host
threads, SPU code being interpreted takes about 1.75 cores, the native SPURS scheduler about 1.4 cores (mostly GTA V's
own policy module on SPUs 0-3 looking for work: the module's code, its reloads and the kernel's selection on
CellSpurs line 0), and the PPU interpreter only about 0.3 cores. The SPU interpreter runs a loop of typical job code
at about 180 MIPS on the M4 (`test_spu "[.bench]"`). So SPU speed (lifting SPU programs, D-029) matters more than PPU
speed for loading; making the per-instruction support check inline gave no measurable gain and was left out.
