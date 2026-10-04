# Run 060 · Depth converted on the GPU: the first mission at 22 to 25 fps

2026-10-04 · 66b8b7a (D-088) · M4 Mac · `pspocket-boot ... --timeout 280 --frames inventory/gta5/frames-060
--frame-every 1500 --trace-frame 3000 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu
--press cross@45 ... --press cross@90` (inventory/boot-060.log).

| Stretch | fps | Run 059 |
|---|---|---|
| The first mission | 22 to 25 | 6 to 7 |

- The traced mission frame's HDR lighting target stayed black: three of its fragment programs use IFE and two UPB,
  and their draws were skipped (D-089).
- The PPU, interpreted, took about two cores; decode()'s switch was a quarter of the PPU threads' time (D-091).
