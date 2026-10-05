# Run 093 · GTA V as a game library, made with the phone's compiler, through the gate (D-152)

2026-10-05 · working tree on 567ca42 (runtime/game, codegen's build, pspocket-prepare, `--game-library`) · M4 Mac.

```
pspocket-prepare --title BLES01807 --exe EBOOT.elf --ppu inventory/gta5/ppu-lifted --spu inventory/gta5/spu-lifted \
  --out inventory/gta5/library/game.dylib -j 10
```

| | |
|---|---|
| Files | 513 PPU, 301 SPU (150 programs), the entry: 815 |
| Compile | 8 min 21 s on ten threads (thread time 83 min; predicted 4,615 cost-seconds) |
| Link | 1.2 s (lld, in process) |
| Library | 145 MB, signed ad hoc |

The time left as the build said it, against what it was:

| At | Said | Was | |
|---|---|---|---|
| start | 7:42 | 8:21 | -8% |
| 10% of the time | 5:43 | 6:52 | -17% |
| 25% | 6:13 | 6:15 | -1% |
| 50% | 4:56 | 4:10 | +18% |
| 75% | 2:31 | 2:04 | +22% |

One rate for every file is the weak point: the big SPU programs come first and take longer than predicted, which
makes the PPU units after them look slower than they are. Next: a rate per kind of file.

The first build stopped at 681 of 815 files (an empty define of ours turned `-fvisibility=hidden` into a macro name;
fixed, with a test); the next run would have compiled only the rest (the objects are kept), but the ABI hash changed
in between, so it started over.

`scripts/gate.sh game --library inventory/gta5/library/game.dylib`:

```
[info] game: game library inventory/gta5/library/game.dylib (BLES01807, made by pspocket 0.1.0 (pspocket-prepare)):
       85376 PPU functions, 150 SPU programs; signature registered, library validation passed
PASS  fps, median of the 5 s windows                          29.8  (>= 28)
PASS  fps, slowest 5 s window                                 29.2  (>= 24)
PASS  frame jumps (64x36 mean difference > 25)                   2  (<= 2)
PASS  GPU ms per frame, worst window in play                  8.76  (<= 30)
PASS  render passes per frame, worst window in play           84.1  (<= 90)
PASS  process memory, peak (MB)                               1616  (<= 2500)
gate: PASS
```

The same numbers as the built-in build's gate of the same day (29.8 / 29.2 fps, 2 jumps, 84.1 passes, 1,605 MB).
CPU over the walk (5 s windows, loading left out): 11.97 CPU-seconds per window with the library against 11.68 built
in, SPURS workers 9.08 against 8.85 s: within 3%, the size of the noise between runs. Stage 3's acceptance is met:
GTA V runs from a library made with the compiler the phone has.
