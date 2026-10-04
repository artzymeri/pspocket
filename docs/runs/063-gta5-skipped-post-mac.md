# Run 063 · The post-processing chain skipped: vertex texture fetch

2026-10-04 · 93dd06e+ · M4 Mac · `pspocket-boot ... --timeout 200 --frames inventory/gta5/frames-063 --frame-every 3000
--trace-frame 3000 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu --press cross@45 ...
--press cross@90` (inventory/boot-063.log).

- The traced mission frame skipped five draws, all for their vertex programs: the post-processing chain (into the
  target the final composite shows, the HDR downsample, a bloom target and the 1x1 exposure value), whose vertex
  programs read the exposure through TXL. That is why the frame showed the G-buffer albedo instead of the lit image
  (D-093).
