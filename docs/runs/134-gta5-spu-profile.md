# Runs 134–141 · GTA V's SPU time, profiled: conversions, calls, byte rotates, shuffles, register traffic

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

## Run 138: inlining small leaves, not kept

Inlining leaf helpers of up to 32 instructions into their callers (the fuzz tests passed): GTA V's main SPU threads
6.98 to 6.97 s per 5 s, nothing. Since D-216 such a call stores and loads only a few registers. Reverted. A second
profile after D-217 (run 137b): untagged code (the C compiler's own spills of SPU registers it cannot keep in 32 NEON
registers) 14%, shufb 11%, calls 9.5%, function entry loads 9.4%, quadword loads 12%, fma 4.6%.

## Run 139: Assassin's Creed with D-215 to D-217

Prepared again (157 s, all files: the SPU header changed), the training walk to the crowd: the five SPU threads 15.0
to 13.9 s of CPU per 5 s (-7.6%), the process 22.1 to 20.9, 33 fps; nothing interpreted beyond the module's 184
instructions, nothing unimplemented. spu-check on its prepared library (22 programs, 1,446 trials): no difference.


## Run 140: shufb in five instructions (D-218)

shufb was 11% of the SPU samples in run 137b. Its special control bytes now take one extending table lookup instead of
compares and a select (nine NEON instructions to five). Checked against the old code on 2 million random operands and
every control byte value, then the differential fuzz test with 1,000 contexts per case. GTA V's walk: the main SPU
threads 6.95 to 6.91 s per 5 s, the audio SPU 1.57 to 1.55: within the noise between runs, never slower.

## Run 141: registers loaded where needed, stored where changed (D-219)

Each translated SPU function now loads at its entry only the registers that may be read before they are written, and
each way out stores only those that may have been written (dirty). Over GTA V's 150 programs the entry loads fell to
45% (192,707 registers to 86,743).

Measuring it took three tries, worth remembering:

- The first two runs said the opposite: the main SPU threads 8.0 and 8.3 s per 5 s. A statistics script of mine had
  been left running (57% of a core) and every run since was slower; the old translation measured 8.33 too.
- The first before-and-after swap of the lifted directories rebuilt nothing: `mv` keeps file times, the old C files
  were older than the objects, so ninja compiled only the descriptor files. Every file now gets `touch`ed after a
  swap (150 function units recompiled each way).
- A temporary counter of the run loop's entries (90.4 million in 80 s, the same with both translations) ruled out
  that returns which now leave the function instead of going through its dispatch switch reach the run loop more.

Same session, the stray script stopped, each side built from its own lifted directory:

| | main SPU threads, CPU s per 5 s (60–90 s) | the audio SPU | the process |
|---|---|---|---|
| before (D-217 lift, D-218 shufb) | 6.98, 7.01 | 1.56, 1.57 | 11.9 |
| after (D-219) | 6.53, 6.53, 6.62 | 1.38, 1.39, 1.39 | 11.2 |

The main SPU threads -6%, the audio SPU -11%. A profile at 95–110 s (`sample`): calls (brsl) 9.5% to 6.3% of the
lifted samples, lines with loads only (function entries, after calls) 9.4% to 3.6%, lines with stores only 3.4% to
3.9%. Since run 134 (D-215 to D-219, measured on different days) the main SPU threads use about 17% less CPU and the audio
SPU about 25% less.
