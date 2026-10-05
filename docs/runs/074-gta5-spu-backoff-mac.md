# Run 074 · Where the SPU workers' time goes; backing off empty polls

2026-10-05 · beddc3e+ (D-103) · M4 Mac · `pspocket-boot ... --timeout 140 --frames DIR --frame-every 100000 --rsx-recover
--flash inventory/firmware/dev_flash --press cross@45 ... --press cross@90`, three times: the time split alone, with a
wait on the polled line, and with the sleep (D-103). CPU from `top` at about 110 s, in the first mission.

| | Process CPU | Empty polls per Primary SPU (0 to 3) | Time in them | fps in the mission |
|---|---|---|---|---|
| Spinning | 4.94 cores | 133 to 187 million | 30 to 43 s | 30 |
| Waiting on the polled line | 4.80 | 121 to 160 million | 28 to 39 s | 30 |
| Sleeping 5 to 100 us | 2.62 | 3.9 to 4.0 million | 1.2 to 1.4 s | 30 |

- The Secondary SPURS's SPU and Primary SPU 4 never run the game's policy module; they wait for work.
- "Running workloads" also fell (40 to 32 s on SPUs 1 to 3): some of it was the same polling inside runs that wrote
  memory.
