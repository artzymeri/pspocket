# Run 081 · Where the CPU goes in the first mission

2026-10-05 · a1f7223+ (D-109) · M4 Mac · the intro presses and a walk (`ls-up@100:6 ... ls-up@120:10`), `sample <pid> 10`
at 115 s, then `top`. Per-thread CPU from the samples outside waits.

| Thread | Share of a core |
|---|---|
| SPURS 10016000 SPU 0 (the second SPURS instance) | 22.8% |
| RSX thread (FIFO, Metal encoding, texture checks) | 17.9% |
| SPURS 10015000 SPU 0 to 4 | 10 to 17% each, 72% together |
| PPU main thread | 6.2% |
| PPU RenderThread | 2.9% |
| Two Metal driver threads | 4.5% |
| Audio (RAGE mix, cellAudio, North Audio) | 2.3% |

| Function | Samples in 10 s, before | After (D-109) |
|---|---|---|
| `ps_spu_hash64` (SPU image identification) | 363 | 29 to 30 |
| `WriteTracker::is_dirty` (texture checks) | 262 | 252 to 270 |

- `top`: 157% before, 139% and 150% in two runs after (the scene is not frame-exact between runs).
- The RSX thread's texture checks: 560 to 780 a frame, 33 to 40 MB hashed (the new line in the per-frame report).
- The rest of the SPU workers' time is the game's own SPU code, translated.
