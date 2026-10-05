# Run 095 · GTA V prepared from its executable by the phone's pipeline, on the Mac (D-154)

2026-10-05 · e287990 and after · M4 Mac.

```
pspocket-prepare --title BLES01807 --exe inventory/gta5/decrypt/EBOOT.elf --game-dir inventory/gta5/prepared \
  --spu inventory/gta5/spu-lifted -j 10 --events events-4.csv
```

runtime/prepare did all of it from the decrypted executable: 85,376 functions and 6.1 M instructions translated in
508 units (6.4 s), 811 files compiled (7:27), linked (1.2 s), signed (1.2 s), the manifest written: 7:39 in all,
143.5 MB. `scripts/gate.sh game --library inventory/gta5/prepared/game.dylib` passes: 29.8 fps median, 29.0 slowest,
2 frame jumps, 1,617 MB, CPU over the walk 10.79 s per 5 s (as the other libraries, run 094).

The time left on this run: -4% at the start, -7% at 10% and 25%, +19% at half, +15% at three quarters. By kind of
file, against their predictions: PPU units 0.93, the 24 big SPU programs 1.17, the mid and small ones 0.95-1.06, the
tables 0.49. The big SPU programs finish first; leaning every kind's rate on the overall one made the PPU units,
which are most of what is left by then, look 20% slower than they were. With a rate per kind and a prior of its own,
the same build replays at 6.7% mean error and 14% at worst (it was 9.5% and 20%), and each preparation now keeps its
rates per kind in the manifest, as the next one's priors.

## A PPU-only library, as the phone makes one today

Without `--spu`: 510 files in 3:53 (translate 6.2 s, compile 3:42, link 0.7 s, sign 0.8 s), 97.8 MB; rates PPU 0.90,
tables 0.64. The time left: +90% at the start (the guess made before translating assumed SPU code too; now it counts
85 cost-seconds per MB of executable for PPU code and 23 per MB of lifted SPU C), then +14%, +10%, +7%, +6% from a
tenth of the way on.

The gate with it and the SPU programs built into pspocket-boot: the first walk died a minute in on the EDGE/GFX job
chain (CELL_SPURS_JOB_ERROR_NUM_CACHE: the descriptor at 0x10395a00 held vertex-like data; D-138's log names the
writer: job 0x1d21c00, binary 0x18fcc80, a 0x2000-byte PUT at 0x10394000 26 ms before); the second walk passed (29.8
fps). The same stop as D-130, D-131 and D-138's, seen before route B and with the built-in build: an intermittent
overwrite of the chain's descriptors, not the library. Open in docs/improve.md (stability), with a writer to follow
now.
