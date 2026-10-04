# Run 045 · Time stretching on the Mac

2026-10-04 · c503afa+ (D-075) · M4 Mac · `pspocket-boot ... --timeout 120 --flash inventory/firmware/dev_flash --audio`
(inventory/boot-047.log).

The sound line every 5 s, from the opening animation to the loading screens:

| Stretch of the run | Game's pace | Periods stretched | Slowest tempo | Skipped periods per 5 s |
|---|---|---|---|---|
| Opening animation | 1.07 | 0 | 1.00 | 0 |
| First loading screens | 0.92 to 0.70 | all from the second window | 0.63 to 0.44 | 0 to 34 |
| Heavier loading | 0.45 to 0.42 | all | 0.35 to 0.30 | 105 to 176 |
| Heaviest | 0.29 to 0.20 | all | 0.25 (the floor) | 280 to 315 |

The speaker ran dry 69 times in 120 s (57.6 s played). Without stretching, runs 043's three Mac runs had about 500 in
100 s. No doubled blocks. Below a pace of 0.25 the speaker still runs dry; that stretch needs a faster game, not
slower audio.
