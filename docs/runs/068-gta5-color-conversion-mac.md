# Run 068 · Y16_X16 over a colour target converted on the GPU

2026-10-05 · 9cd1f31+ (D-098) · M4 Mac · `pspocket-boot ... --timeout 150 --frames inventory/gta5/frames-068
--frame-every 1500 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45
... --press cross@90` (inventory/boot-068.log).

- GPU readbacks: only the 80x45 target at 0xcd5b2000, for an NV0039 buffer copy, every frame (2,911); the two 640x360
  targets read as Y16_X16 (run 067: 4,440 each) are converted on the GPU.
- The dependency task still interpreted about 3.9 million instructions at 0x071f0 from 102 entries: its modules'
  runs were replaced by its data before they first ran (D-099).
