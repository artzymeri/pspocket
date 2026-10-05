# Run 094 · Faster compile settings for translated code, and a time left that holds (D-153)

2026-10-05 · working tree on b0e7280 · M4 Mac, `pspocket-prepare -j 10` and `scripts/gate.sh game --library`.

## Settings (GTA V, all 815 files)

| Library | Settings | Compile | Thread time | Size |
|---|---|---|---|---|
| game.dylib | LLVM's defaults (-O2) | 8:21 | 83 min | 144.7 MB |
| game-fast.dylib | PPU `-fno-slp-vectorize`, `-grow-region-complexity-budget=1000` | 6:47 | 67 min | 143.5 MB |

19% less, on top of the slowest PPU unit dropping from about two minutes to seconds (run 092).

The gate walk, each library twice (CPU over the walk's 5 s windows, loading left out):

| Library | fps median / slowest | CPU s per 5 s | SPURS workers s per 5 s |
|---|---|---|---|
| built in (Apple clang 17), earlier the same day | 29.8 / 29.2 | 11.68 | 8.85 |
| game.dylib, run 1 | 29.8 / 29.2 | 11.97 | 9.08 |
| game.dylib, run 2 | 29.8 / 29.2 | 10.65 | 8.00 |
| game-fast.dylib, run 1 | 29.8 / 29.4 | 10.69 | 8.04 |
| game-fast.dylib, run 2 | 29.8 / 29.0 | 10.74 | 8.10 |

The CPU numbers move by 12% between two runs of the same library (the first run after a long compile was the high
one), so they say no more than this: the fast settings run the walk as well as the defaults, at the same frame rate.
Adopted for the compile time alone (`codegen::translated_code_settings`).

## The time left

`pspocket-prepare --events` records every file's start and end; `eta_replay.py` (scratch) replays the record and asks
each way of estimating what it would have said at each point:

| At (of the compile time) | 0% | 5% | 10% | 25% | 50% | 75% | 90% |
|---|---|---|---|---|---|---|---|
| one rate (stage 3's first build) | +13% | +14% | -37% | -16% | +7% | -1% | +2% |
| a rate per PPU/SPU group | +13% | +14% | -43% | -17% | +9% | -4% | 0% |
| per group, the run's true rates known (the floor) | -1% | 0% | -18% | -7% | +8% | -4% | 0% |
| per group, prior weighing 300 cost-seconds | +13% | +14% | -5% | -10% | +8% | -3% | 0% |
| per group, prior weighing 1,000 | +13% | +14% | +7% | -1% | +10% | -1% | +1% |

The dip at a tenth of the way is the first files to finish being the ones that ran fast: trusting them early made the
estimate fall 40% low. A prior that weighs 300 cost-seconds (about 6% of GTA V's) keeps every point within 14%; the
default is now 300, and the cost model is refitted for the new settings (PPU 2.469 s/MB plus a shape term, mean error
14% per file; SPU 31% per file: SPU programs vary more, and a file on an efficiency core takes longer).

## A fresh build with all of it, and the model refitted

A build from scratch with the new defaults took 7:33 (thread time 75 min; the same settings took 6:47 an hour
earlier: the Mac, after an hour of compiling, ran 20% slower). Its time left, cumulative rates with the prior at 300:
-25% at a tenth of the way, -11% at a quarter, +20% at half, +6% at three quarters. Two causes, from the files'
timings:
- **The machine's speed moves within and between builds** (the rate per minute of the build went from 0.67 to 1.73
  for SPU files in this one; the two builds differ by 20% overall). The time left can only follow it.
- **The registration tables are not code.** The lifters' `*_reg.c` and lift-spu.sh's `game_spu.c` (151 files) compile
  in no time but were predicted as code; they all come at the end, where they made the estimate too high.

The model now has three kinds of file (PPU code, SPU code, tables), refitted on both builds with each build's own speed
taken out (PPU 2.599 s/MB + 1.385 per 10^6 lines x labels, 17% per file; SPU 6.767 + 9.180, 31%; tables 0.852 s/MB),
and the prior weighs a quarter of the build's predicted cost, at most 1,000 cost-seconds. Both builds simulated
again with it (their files' real times, laid out in the new order, ten workers):

| Build | Prior | Mean error | Worst | 0% | 10% | 20% | 30% | 50% | 70% | 90% |
|---|---|---|---|---|---|---|---|---|---|---|
| 6:44 | 1.00 | 5% | 11% | +8 | +1 | -9 | -11 | 0 | -1 | -6 |
| 7:30 | 1.00 | 8% | 15% | -3 | -10 | -15 | -8 | +12 | +7 | +7 |

The time left is now within 15% from the first second to the end on both (fitted on these builds: a new one will do
a little worse). On the phone the prior comes from the phone's own last build (stage 4) or 1.0: its cores compile at
the M4's pace (run 092).
