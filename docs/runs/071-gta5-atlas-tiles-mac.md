# Run 071 · Every shadow cascade from its depth buffer

2026-10-05 · ed07f03- (D-100) · M4 Mac · `pspocket-boot ... --timeout 150 --frames inventory/gta5/frames-071
--frame-every 1500 --trace-frame 3000 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu
--press cross@45 ... --press cross@90` (inventory/boot-071.log).

- All DEPTH16 textures of the traced frame are converted depth buffers (nine 128x128 cascades, five 256x256), none
  from memory.
- 29 to 30 fps in the mission.
