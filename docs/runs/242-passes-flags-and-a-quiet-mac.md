# Runs 242–257 · Render pass ends, event flags, and a Mac that was not quiet (D-246, D-248, D-249)

2026-10-08 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt` (128 s).
Runs 242-245 under load and profiled at 70 s; 246-257 idle and unprofiled. Logs in inventory/gta5 (kept out of the
repository).

## An iOS Simulator in the background

Runs 242-247 dipped to 4-21 fps for 5-10 s at a time, with the old binary as with the new. Two causes:

- `sample` stops the process for about 5 s the first time it profiles a binary it has not seen (reading its symbols):
  the window around 70 s of each new binary's first profiled run stretches to 8-11 s at 1-5 fps (runs 234, 235, 239,
  243). The run notes before compared such windows too; they are skipped from here on (windows longer than 5.5 s and
  the one after).
- An iOS Simulator had been running PSPocket for almost three days: about 0.9 of a core (backboardd, SimMetalHost) and
  GPU time. Shut down (`xcrun simctl shutdown all`); the idle walks since hold 29-30 fps throughout.

On the quiet Mac, identical binaries still differ by up to 8% in CPU per frame from run to run (runs 248-257), so a
change has to save more than that to show in a pair of runs; profiles and counters show smaller ones.

## What ends GTA V's render passes (runs 243, 245, counted)

Of the passes ended by a target change (about 55-70 a frame): 84% switch to other colour targets, 14% keep the colour
targets with another depth buffer, 2% are a draw without depth after one with it (which could keep the pass, not
worth its hazards), none a size or guest-memory change alone.

Corner copies (4-6 a frame in the scene after 120 s): every one is the top-left 256x128 of a 512x256 target that the
same pass is drawing to. The copy must hold what the pass drew so far, so copying earlier (D-248) cannot help:
measured, not kept.

## Event flags (D-246)

EventFlag::set is 1.0-1.3% of the SPU threads' samples. Without allocations: no measurable change (idle CPU per frame
0.0620-0.0761 s without, 0.0646-0.0730 with). Not kept; its twenty-waiter test stays.

| run | binary | fps | CPU per frame, s | main SPU threads per frame | RSX thread per frame |
|---|---|---|---|---|---|
| 248 | before | 29.7 | 0.0761 | 0.0450 | 0.0096 |
| 249 | D-246, D-248, D-249 | 29.8 | 0.0646 | 0.0378 | 0.0083 |
| 250 | before | 29.3 | 0.0709 | 0.0417 | 0.0091 |
| 251 | D-246, D-248, D-249 | 29.8 | 0.0658 | 0.0384 | 0.0085 |
| 252 | before | 29.9 | 0.0620 | 0.0362 | 0.0080 |
| 253 | HEAD (d73d974) | 29.9 | 0.0645 | 0.0373 | 0.0083 |
| 254 | D-246, D-248, D-249 | 29.7 | 0.0681 | 0.0395 | 0.0087 |
| 255 | before | 29.7 | 0.0666 | 0.0387 | 0.0085 |
| 256 | HEAD (d73d974) | 29.9 | 0.0623 | 0.0360 | 0.0080 |
| 257 | D-246, D-248, D-249 | 29.9 | 0.0730 | 0.0425 | 0.0097 |

## Primitive restart (D-249)

Not used by GTA V or Assassin's Creed (neither log has ever named it); checked by its synthetic test.
