# Run 067 · Modules found in shared blocks; the script VM translated; DEPTH16 from memory

2026-10-05 · 9cd1f31+ (D-095 to D-097) · M4 Mac · `pspocket-boot ... --timeout 200 --frames inventory/gta5/frames-067
--frame-every 1500 --rsx-recover --flash inventory/firmware/dev_flash --press cross@45 ... --press cross@90`
(inventory/boot-067.log).

- The first mission at 28 to 30 fps throughout; the frames show it lit and complete (frame 4,500: the bank, the
  player and a gunman, the radar).
- The dependency task's most interpreted address: 4 million instructions from 119 entries (run 066: 201 million from
  488,146).
- No DEPTH16 errors. Left on the RSX's list: a 3D texture, MSAA (AA mode 3) drawn with one sample, conditional
  rendering, occlusion queries (reporting 65535).
