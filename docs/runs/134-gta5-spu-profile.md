# Runs 134–137 · GTA V's SPU time, profiled: conversions, calls, byte rotates

2026-10-07 morning · pspocket-boot with GTA V's built-in translation (PPU re-lifted on 2026-10-07, SPU programs as
noted) · M4 Mac · GTA V's prologue walk (`games/gta5/walk-prologue.txt`, 128 s, Metal), macOS `sample` on the running
process for 15 s from 95 s.

## Where the time goes (run 134)

Busiest threads: the second SPURS instance's one SPU (GTA V's audio mixer) 51%, the five main SPU threads about 39%
each, the RSX thread 36%, the main PPU thread 16%. The main SPU threads' busy time: one geometry job program
(spu_dad457863220b1cc) 35%, another 10.6%; the module tracker's GET observer only 3.1% (it is 11% in Assassin's
Creed's crowd). Summed by SPU instruction (each translated C line follows a comment naming its instruction):

| instruction | share of SPU samples |
|---|---|
| lines with no instruction (function entry loads, exit stores, the dispatch switch) | 22.1% |
| brsl (calls: the stores and loads around them) | 13.5% |
| cflts (float to integer) | 8.5% |
| shufb | 7.0% |
| rotqby | 5.3% |
| lqx, lqd | 9.0% |
| fma | 3.7% |
| stqd | 3.1% |
| bi (returns) | 2.6% |

The hottest lines of the geometry job are cflts between shuffles: the lifted code converted each lane through double
precision.

## The changes

| run | | main SPU threads, CPU s per 5 s (60–90 s) | the audio SPU | the process |
|---|---|---|---|---|
| 134 | before | 7.93 | 1.85 | 13.3 |
| 135 | D-215: cflts and cfltu in NEON | 7.62 | 1.88 | 13.0 |
| 136 | D-216: a call stores and reloads only what the callee can touch | 7.33 | 1.73 | 12.4 |
| 137 | D-217: byte shifts and rotates in NEON | **6.98** | **1.57** | **11.9** |

D-215 was checked with the differential fuzz test and with spu-check on GTA V's 53 programs that use cflts or cfltu
(2,679 trials, no difference). D-216 brings back D-127's call saves (withdrawn in D-131, though D-138 later saw the
same EDGE stops without them): the program-level fuzz test passes, the gate passes, and spu-check over all 150 of GTA V's
programs compared 5,002 trials with no difference. What is left of the call overhead is each function's own loads at entry and stores at exit (all the
registers it uses and writes).

D-217 (byte rotates) was checked the same way: fuzz, spu-check over all 150 programs (5,002 trials, no difference), the
gate. All three together: the main SPU threads 12% less CPU, the audio SPU 15% less, the process 11% less.
