# Run 051 · Frame rates on the Mac with lifted SPU programs

2026-10-04 · a532a5f+ (D-079) · M4 Mac · `pspocket-boot ... --timeout 420 --gpu metal --flash inventory/firmware/dev_flash
--spu-dump inventory/gta5/spu --press cross@45 ... --press cross@90` (inventory/boot-051.log). The fps line every 5 s.

| Stretch | fps |
|---|---|
| Opening animation | 11 to 13 |
| First loading screens | 29 to 30 (the game's own cap) |
| "Loading Story Mode" | 8 to 10 |
| The first mission | 3.6 to 4 |

SPU program loads: 1,256,279 lifted, 409 interpreted (25 images without a lifted program). A Time Profiler capture of
the mission: the SPU interpreter still takes about 38% of the workers' time, nearly all of it in the modules
`dependency_spu` loads (D-079 found them; none was lifted yet); SPURS's workload selection about 16%, mutexes about 14%.
