# Run 333 · 2026-10-16 · Assassin's Creed's first install on the Mac bench

- Build: 1d2a97b (main), pspocket-boot on the M4 Mac, from a library pspocket-prepare made (as the phone does), the
  installed system software's dev_flash. scripts/bench.py install-garden --game ac1 (D-1063).
- The case: an empty game folder and no save (the iPhone's freeze, D-999), 330 s, presses from
  games/ac1/walk-garden.txt; play counted from 120 s.

| | |
|---|---|
| Verdict | PASS (no failures, no warnings; first run, no baseline yet) |
| Exit | the timeout at 330 s, 11,701 flips |
| fps in play | 26.9 (the install's loading screens 17 to 19, the garden 56 to 60) |
| CPU a frame | 64.6 ms (workload 10: 26.1, workload 1: 17.0, workload 6: 11.0, workload 8: 9.5, RSX 6.7) |
| Draws a frame | 777 |
| RSX failures | 0 |
| Partly black frames | 0 of 195 checked |

The phone's freeze on the first install does not happen on the Mac with D-999 (the race fixed there). The training case
(from the save) is next; both now run every night (D-1067).
