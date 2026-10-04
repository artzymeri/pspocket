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
