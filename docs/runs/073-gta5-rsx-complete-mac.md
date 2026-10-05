# Run 073 · One RSX failure in the run

2026-10-05 · fd3a099+ (D-102) · M4 Mac · `pspocket-boot ... --timeout 150 --frames inventory/gta5/frames-073
--frame-every 1500 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45 ...
--press cross@90` (inventory/boot-073.log).

- The RSX: 4,321 flips, 1 failure (the multisampled target, once), 4 draws skipped by conditional rendering. 3D
  textures, occlusion queries and conditional rendering run.
- 28 to 30 fps in the mission; frame 3,000 as in run 071.
