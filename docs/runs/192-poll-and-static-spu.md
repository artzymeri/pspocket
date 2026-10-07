# Runs 192–207 · The SPURS poll on a plain copy (tried); the executable's SPU programs translated when preparing (D-226)

2026-10-07 · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt` (128 s; "under load": six threads spinning at
the SPU threads' QoS class); Assassin's Creed (BLES00158), `games/ac1/walk-training.txt` (150 s). Logs in inventory/.

## Polls on a plain copy of the SPURS line (runs 192-205, not kept)

| runs | | before (D-225) | the change |
|---|---|---|---|
| 192-195 | idle, main SPU threads s per 5 s | 7.21, 7.16 | 7.05, 7.34 |
| 196-199 | under load | 8.58, 6.65 (22 fps: the Mac busy elsewhere) | 8.34, 8.21 |
| 200-203 | under load | 8.45, 8.96 | 7.88, 6.86 (25 fps) |

Too noisy under load to tell; the profiles (runs 204, 205, under load, 15 s) show the select1 yields not gone (142 to
217 samples): they come from the dispatch's selection and from DMA, not the polls. Reverted. The SPU threads' yields
are down to 1.4% of their samples since D-223 and D-225 (run 204).

## A fresh profile (run 206, idle)

Of the main SPU threads' translated code: untagged lines (the compiler's spills) 10.6%, shufb 10.3%, function entries
8.0%, lqx 7.5%, brsl 6.6%, fma 5.6%, lqd 5.3%, exit stores 4.8%. Outside it: memmove 1.3%, ps_res_notify_write 0.7%,
the tracker's release 0.5%.

## The executable's SPU programs (D-226, run 207)

Assassin's Creed prepared with no saved images: 6 SPU programs from the executable's 6 SPU ELFs, the 4 tasks under the
names its runs saved (spu_26f34c371d06dc23_03000, spu_2fed95d316ff1968_03000, spu_a6e65c3fd3b22009_03000,
spu_bb5ecc9a5f6a2f73_03000) and 2 position independent ones at 0x80 (relocatable; never met in our walks). Prepared in
152 s (all 381 files compiled: the copied folder's objects were not reused at a new path).

A 150 s session on that library (run 207): spu_2fed95d316ff1968_03000 found translated each time (13 mentions, none
"has no lifted program"). The SPU code still interpreted is what that task copies in from data: 19.0 billion
instructions at LS 0x2fba0, 2.6 billion at 0x04c00 ("in no program"), and 32 images without a lifted program; the full
library (from saved images) has those as modules and jobs. The gate passes.
