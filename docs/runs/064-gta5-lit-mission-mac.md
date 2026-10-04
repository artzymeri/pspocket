# Run 064 · The first mission lit correctly, at 26 to 28 fps

2026-10-04 · 9cd1f31 (D-093) · M4 Mac · `pspocket-boot ... --timeout 200 --frames inventory/gta5/frames-064
--frame-every 1500 --trace-frame 3000 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu
--press cross@45 ... --press cross@90` (inventory/boot-064.log).

- No skipped draws in the traced frame; the mission's frames show the lit, post-processed image instead of the pink
  albedo of runs 050 to 063.
- The first mission at 26 to 28 fps, the PPU still interpreted.
