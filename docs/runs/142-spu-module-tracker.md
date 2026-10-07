# Runs 142–149 · The SPU module tracker by line owners (D-220); Assassin's Creed with D-219

2026-10-07 · pspocket-boot with the Metal backend · M4 Mac · Assassin's Creed (BLES00158), `games/ac1/walk-training.txt`
(600 s, the "Gentle Push" crowd at the end), and GTA V (BLES01807), `games/gta5/walk-prologue.txt` (128 s). Before and
after in one session, alternating, from two copies of pspocket-boot built from the same tree (the committed tracker and
D-220), under `caffeinate`; logs and profiles in inventory/ (kept out of the repository).

## Run 142: Assassin's Creed with D-219

The last session's measurement of D-219 on Assassin's Creed (the Mac had slept during the first try). Against run 139
(D-215 to D-217), the same walk: the five SPU threads over the last 12 windows 10.17 to 9.39 s of CPU per 5 s (-7.7%), the
process 16.07 to 15.47, the fps the same (median 58.7, 39.9 over the last 12); SPU code interpreted 184 instructions,
nothing unimplemented, as before.

## The tracker

The GET observer of the module tracker (D-079) was 11-12% of the SPU threads' busy time in Assassin's Creed's crowd (runs
126-127): each GET tested the runs listed in the 1 KB blocks it covered and cut the ones it wrote over. D-220 gives each
16-byte line of local store the run that wrote it last: a GET takes its lines from their runs and either extends the
last run or starts one; the run holding pc is its line's, and the part still in local store the lines around pc it
holds. A run lives while it holds a line (a draft that kept only the newest 4,096 runs would have forgotten a module
loaded once and entered again after thousands of other copies: a regression test now covers it), and forgetting
everything at a task switch is one increment.

## Runs 143–145: Assassin's Creed's crowd

"Last 12 windows" mixes the quiet windows before the crowd (about 2 s of SPU time per 5 s) with the crowd (about 13 s),
and the crowd starts a window earlier or later from run to run; the table counts the crowd windows only (SPU time over
10.5 s, the first one left out as partial). Runs 144 and 145 were profiled with `sample` at 555-570 s.

| run | tracker | crowd windows | 5 SPU threads, CPU s per 5 s | process | fps |
|---|---|---|---|---|---|
| 142 | before | 7 | 13.16 | 20.16 | 33.1 |
| 143 | D-220 | 7 | **11.59** | 18.54 | 37.2 |
| 144 | before (profiled) | 5 | 14.56 | 21.90 | 30.7 |
| 145 | D-220 (profiled) | 5 | **12.79** | 19.70 | 30.2 |

The SPU threads -12% in both pairs, the process -8 to -10%. Every run: 2.0 to 2.1 million SPU program loads lifted, 0
interpreted, 56,400 to 57,100 modules lifted, 184 SPU instructions interpreted (the one module entry known since D-208),
nothing unimplemented.

The profiles fell mostly before the crowd (it began at about 565 s in both). There the tracker's own time is about the
same either way (self samples: the old observer and its block helpers about 1,050, 3.4% of the SPU threads' samples; the
new one's release and new_run about 1,300, 3.7%): a 16 KB GET walks 1,024 lines twice where the old one walked 16
blocks. The crowd's many small GETs are where it gains.

## Runs 146–149: GTA V

The prologue walk (the gate's), 60-90 s, alternating:

| run | tracker | main SPU threads, CPU s per 5 s | audio SPU | process | fps |
|---|---|---|---|---|---|
| 146 | before | 8.07 | 1.72 | 13.6 | 29.8 |
| 147 | D-220 | 7.12 | 1.52 | 12.1 | 29.5 |
| 148 | before | 6.54 | 1.38 | 11.2 | 29.9 |
| 149 | D-220 | 6.80 | 1.66 | 11.4 | 29.6 |

Within the noise between runs: the first run after 40 minutes of Assassin's Creed ran warm (8.07; the last session
measured 6.53-6.62 on the same walk), and GTA V's SPU code copies far less than Assassin's Creed's crowd (the old observer
was about 3% of its SPU time, run 127). Every run: 688,000 to 723,000 modules lifted, none interpreted, nothing
unimplemented, no job chain stop. The gate passed with D-220 (below).
