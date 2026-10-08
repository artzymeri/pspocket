# Runs 234–241 · Two SPU-side tries measured and not kept (D-245, D-247)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt` (128 s;
the 60-90 s windows), all under load (six threads spinning at the SPU threads' QoS class), each run profiled with
`sample` for 15 s at 70 s. Before and after alternate in one session. Logs in inventory/gta5 (kept out of the
repository).

CPU per frame is the process's CPU seconds in a 5 s window over the frames in it: under load the frame rate moves
from run to run (22 to 29 fps), and the work follows the frames.

## Waiters on a line's version word (D-245, runs 234-237)

| | before (234, 236) | after (235, 237) |
|---|---|---|
| fps (median of the windows) | 23.8, 28.8 | 25.2, 28.5 |
| CPU per frame, s | 0.103, 0.102 | 0.106, 0.101 |
| the main SPU threads per frame, s | 0.058, 0.058 | 0.060, 0.058 |
| samples: pthread_cond_broadcast / __ulock_wake | 103, 244 / 0, 0 | 57, 173 / 109, 325 |
| samples: the wake's mutex waits (ps_res_store_line) | 40 | 0 |

The wake's lock went, the wake's system call did not get cheaper, and nothing moved per frame. Not kept.

## A stop as one more way out (D-247, runs 238-241)

| | before (238, 240) | after (239, 241) |
|---|---|---|
| fps | 26.0, 26.1 | 22.3, 27.5 |
| CPU per frame, s | 0.1000, 0.0992 | 0.1028, 0.0939 |
| the main SPU threads per frame, s | 0.0562, 0.0557 | 0.0572, 0.0538 |
| translated SPU time: calls (brsl) | 6.6%, 6.3% | 6.0%, 6.6% |
| entry loads | 7.2%, 7.4% | 7.2%, 7.3% |
| exit stores | 5.2%, 5.1% | 5.6%, 5.3% |

Statically 3% fewer stores before calls; spu-check found no difference over all 150 programs (102,098 trials). Not
kept; the fuzz program's stops in callees stay as tests.

## Where the SPU time goes now (run 237)

The main SPU threads' translated code by SPU instruction: untagged lines (the C compiler's own spills, the dispatch)
10.9%, shufb 9.4%, entry loads 6.9%, lqx 6.8%, lqd 6.4%, fma 6.0%, calls 6.0%, exit stores 5.3%, stqd 4.4%. By program:
the EDGE geometry job (its strings name skinning and culling) is 35% of the translated SPU time, the next four 17%,
7%, 6% and 4%. Of the calls' samples, 30% are calls that store 60 registers or more.

## The RSX thread (run 237)

78% of its time in draws; render command encoders (one per render pass, about 70 a frame: 53-55 ended by a target
change, 5 by feedback copies, 6 by corner copies, 4 by depth and colour conversions) 7.7%, texture binds 8%, fragment
programs 7.5%, target selection 7%.
