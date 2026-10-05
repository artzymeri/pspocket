# Run 070 · Shadow lookups compare depth; the cascades read stale memory

2026-10-05 · ed07f03- (D-100 in progress) · M4 Mac · `pspocket-boot ... --timeout 150 --frames inventory/gta5/frames-070
--frame-every 1500 --trace-frame 3000 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu
--press cross@45 ... --press cross@90` (inventory/boot-070.log).

- The traced frame bound DEPTH16 textures on unit 14: the 128x128 shadow cascades at 0xcf100000 + 0x100 k, all from
  memory, and some 128x128 and 256x256 depth buffers converted. The cascades are tiles of one depth atlas, each tile's
  surface replacing the one before (D-100).
- The first mission at 26 to 30 fps (the frame trace slows "Loading Story Mode").
