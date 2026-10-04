# Run 053 · Relocatable jobs: the intro twice as fast; the modules saved once each

2026-10-04 · 750963c (D-081) · M4 Mac · `pspocket-boot ... --timeout 420 --gpu metal --flash inventory/firmware/dev_flash
--spu-dump inventory/gta5/spu --press cross@45 ... --press cross@90` (inventory/boot-053.log), the job binaries lifted
relocatable (36 programs where run 051 had 102 per-address ones), the modules not lifted yet.

| Stretch | fps | Run 051 |
|---|---|---|
| Opening animation | 13, then 20 to 28 | 11 to 13 |
| First loading screens | 29 to 30 | 29 to 30 |
| "Loading Story Mode" | 7.4 to 9.8 | 8 to 10 |

- The modules were saved once each: 44 images (run 052: 32,570 before it was stopped).
- SPU program loads: 3,989,452 lifted, 899,501 interpreted (the 44 modules); run 051 had 1,256,279 loads in the same
  420 s, the SPUs did three times the work.
- The first mission had not started at 420 s (5,317 frames; run 051 started it at about 315 s, 4,816 frames by
  420 s). How long "Loading Story Mode" takes varies between runs (run 052 had not left it at 400 s either); run 054
  saves frames to see where it is.
