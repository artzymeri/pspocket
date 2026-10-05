# Run 075 and 076 · The RSX thread's time: texture hashing; the render thread's yields

2026-10-05 · 8666ca9+ (D-104) · M4 Mac · `pspocket-boot ... --timeout 140 --frames DIR --frame-every 1500 --rsx-recover
--flash inventory/firmware/dev_flash --press cross@45 ... --press cross@90`, sampled with `sample PID 2` at about 110 s
in the first mission.

| | Process CPU | RSX thread busy | is_dirty | Render thread yielding |
|---|---|---|---|---|
| Run 075 (after D-103) | 2.6 cores | 967 / 1,364 samples | 447 | 72 / 163 busy |
| Run 076 (D-104) | 2.2 to 2.4 cores | 583 / 1,400 | 70 | 7 / 175 (another sample) |

- The SPU workers are busy 40 to 46% each, in the game's jobs (translated), module hashing (ps_spu_hash64, 1 to 2%)
  and waking reservation waiters (1%).
- 30 fps in the mission throughout; frames as before.
