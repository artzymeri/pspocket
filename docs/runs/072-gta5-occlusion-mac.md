# Run 072 · Occlusion queries counted on the GPU

2026-10-05 · fd3a099+ (D-102 in progress) · M4 Mac · `pspocket-boot ... --timeout 170 --frames
inventory/gta5/frames-072 --frame-every 1500 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump
inventory/gta5/spu --press cross@45 ... --press cross@90` (inventory/boot-072.log).

- Occlusion reports come from the GPU's counters; nothing in the frames disappeared. 29 to 30 fps in the mission.
- The RSX still counted 361,371 failures: the multisampled 128x128 target reported at every draw, and a 3D texture
  (64x64x32 R5G6B5, swizzled) at every bind (D-102).
