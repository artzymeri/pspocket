# Runs 172–191 · GTA V's EDGE/GFX job chain stop, found and fixed (D-224, D-225)

2026-10-07 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt` (128 to
150 s); "under load": six threads spinning at the SPU threads' QoS class. Logs in inventory/gta5 and
inventory/repro-edge2 (kept out of the repository).

## The stop, more often

The job chain stop (CELL_SPURS_JOB_ERROR_INVALID_BINARY or NUM_CACHE on a descriptor holding other data, the chain
halting) had come about once in ten walks under load (inventory/repro-edge, 2026-10-06). A measurement of another change
(runs 172-179) stopped three times in eight, all 60-90 s into the walk; twelve walks without load did not stop
(repro-edge2 runs 2-13), so the load matters, as on the iPhone, where the cores are always full.

D-222's grab snapshot showed the descriptor already broken when the chain took the job (not changed after the grab).
D-224 made the stop's report list the failing batch and the chain's last 64 visits.

## What the visits showed (repro-edge2 run 26, under load)

| visit | pc | commands read | taken |
|---|---|---|---|
| 3.9 to 3.7 ms before | 0x1074ca00 | END, then the previous lap's JOBs | waits |
| 0.425 ms before | 0x1074ca00 | JOB 0x10173e00, END | 1 job, pc to ca08 |
| 0.307 ms before | 0x1074ca08 | **0x12** (a light sync), then the previous lap's JOBs 0x1014ba00, b800, b600 | passes the sync |
| 0.303 ms before | 0x1074ca10 | JOBs 0x1014ba00, 0x1014b800, 0x1014b600, 0x1014de00 | 4 jobs: the stop |

Memory afterwards: 0x1074ca08 a JOB (0x10177e00), 0x1074ca10 an END. The batch's descriptors: garbage, floats, zeros,
and one whole. The game was turning the END at ca08 into a jump-to-self (0x00000008_00000012) to append to the list.
Its jobs write job chain commands with 8-byte PUTs, and since D-124 every PUT longer than a word wrote its first word
last: the low word 0x12 landed alone, a light sync, for as long as the SPU thread was preempted between the two stores.

## The fix and the check

D-225: a PUT writes all but its first eight bytes, then those eight in one release store. A test PUTs a jump-to-self and
an END alternately into a doubleword while another thread reads it whole: halves seen in 3 of 3 runs without the fix,
none with it.

| | walks under load | stops |
|---|---|---|
| before (repro-edge2 run 1, runs 176, 178, repro-edge2 runs 20-26) | 10 | 3 |
| D-225 (repro-edge2 runs 41-51, 150 s each) | 11 | **0** |

Run 40 (D-225) ended after 20 s of play: the process's clock jumped 600 s, by the look of it the Mac slept; not a
stop. The gate passes. An adversarial review of the change (four lenses, two refuters per finding, 32 agents) left no
finding standing; three suggestions were taken anyway (the test waits for its reader and checks that every PUT landed;
the report's times are signed; a cut command list is marked).
