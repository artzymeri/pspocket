# Run 054 · Every SPU program lifted: the intro at 30 fps, the first mission at 9 to 10

2026-10-04 · 750963c+ (D-081) · M4 Mac · `pspocket-boot ... --timeout 600 --frames inventory/gta5/frames-054
--frame-every 300 --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45 ... --press
cross@90` (inventory/boot-054.log), with run 053's 44 modules lifted (80 programs, 64 MB of C).

| Stretch | fps | Run 051 |
|---|---|---|
| Opening animation | 15, then 30 (the game's cap) | 11 to 13 |
| First loading screens | 29 to 30 | 29 to 30 |
| "Loading Story Mode" | 8 to 12 | 8 to 10 |
| The first mission (on screen by frame 2,400) | 8.6, rising to 10.0 | 3.6 to 4 |

- SPU program loads: 9,423,986 lifted, none interpreted; 1,687,573 module loads found their lifted program. No new
  image appeared in 10 minutes.
- 4,970 addresses still had no lifted code, nearly all in the modules `dependency_spu` loads: their loads that the task
  changes in local store before they run, which are interpreted (D-079's check). Run 055 counts them.
- The mission's world is drawn, but in pink, magenta and yellow (frames 2,400 to 6,900): D-080 did not fix the
  colours. Gaps still logged: vertex texture fetch (TXL) and BRI/ARL in vertex programs, UPB in a fragment program,
  DEPTH16 textures, readback of B8 targets, and 2,255 write-backs of an 80x45 target that a texture reads in another
  format (each frame).
