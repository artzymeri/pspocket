# Run 066 · The game's PPU code translated: the first mission at the 30 fps cap

2026-10-05 · 9cd1f31+ (D-094) · M4 Mac · `pspocket-boot ... --timeout 200 --frames inventory/gta5/frames-066
--frame-every 1500 --rsx-recover --flash inventory/firmware/dev_flash --press cross@45 ... --press cross@90`
(inventory/boot-066.log), with GTA V's 85,370 PPU functions translated ahead of time and built in.

| Stretch | fps | Run 064 (PPU interpreted) |
|---|---|---|
| Opening and loading screens | 29 to 30 | 29 to 30 |
| "Loading Story Mode" | 20 to 23 | about 20 |
| The first mission | 28 to 30 (the game's cap) | 26 to 28 |

- No fault in three 25 s runs or this one: D-094's entry fix holds.
- One SPU dependency task (spu_5d53598703d84caa) interpreted 201 million instructions at LS 0x071f0 and 7,596 other
  addresses of the modules it loads; every task syscall missed at 0xA70 (D-095).
- The script VM fell back to the interpreter at its second dispatch every time (`indirect jump to 0x0091cb64 has no
  lifted function`, D-096).
- Two 640x360 targets and one 80x45 target were read back from the GPU 4,440 times each (D-098); 35 DEPTH16 textures
  could not be decoded from memory (D-097).
