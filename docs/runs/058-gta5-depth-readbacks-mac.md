# Run 058 · Jobs lifted again; a third of one job binary unreached; 65,433 depth readbacks

2026-10-04 · 37f47f9+ (D-085) · M4 Mac · `pspocket-boot ... --timeout 360 --frames inventory/gta5/frames-058
--frame-every 900 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45 ...
--press cross@90` (inventory/boot-058.log).

- One job binary was 36% lifted: the rest is reached through function pointers. Its misses, logged in its own terms
  (D-081), seeded the next lift, which reached 86% of it.
- GTA V's policy module left through the kernel's exit at 0x808 2.1 million times a second, each time a run-loop miss
  that interpreted one `stop` (D-087 gives the planted stops lifted stand-ins).
- 65,433 readbacks of depth buffers that textures read (the main depth buffer and the shadow maps, every frame): the
  first mission fell from 9 to 6.6 fps (D-088 converts them on the GPU).
