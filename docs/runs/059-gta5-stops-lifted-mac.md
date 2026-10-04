# Run 059 · The worker's stops lifted; the most interpreted address down to 70 million

2026-10-04 · db6b7ad+ (D-085 to D-087) · M4 Mac · `pspocket-boot ... --timeout 300 --frames inventory/gta5/frames-059
--frame-every 1200 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45 ...
--press cross@90` (inventory/boot-059.log).

- The most interpreted SPU address ran 70 million instructions in 300 s (run 057: 16.6 billion).
- The first mission at 6 to 7 fps: the depth readbacks of run 058 still dominate.
- The phone session that evening (the user's playtest: a pink world at about 6 fps, broken sound, no people) is what
  D-086 refers to: every skinned draw was skipped for its vertex program's ARL and relative constants.
