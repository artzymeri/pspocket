# Run 065 · The game's PPU code translated: a fault within seconds

2026-10-04 · 9cd1f31+ (D-092, `-DPSPOCKET_GAME_PPU`) · M4 Mac · `pspocket-boot ... --timeout 240 --frames
inventory/gta5/frames-065 --frame-every 1500 --rsx-recover --flash inventory/firmware/dev_flash --spu-dump
inventory/gta5/spu --press cross@45 ... --press cross@90` (inventory/boot-065.log), with GTA V's 85,342 PPU functions
translated ahead of time and built in.

- The main thread faulted early: `SIGBUS: guest access to 0x00000000 ... at cia 0x013a0ad8 (lr 0x13a08a4)`. The cia is
  stale in translated code, and the fault's address and position changed from run to run (0x0, 0x2, 0x4, 0x6d), which
  misled a bisection over the translated functions.
- The host frames (D-094) named the function: a wrapper entered above the shared body it branches into, translated to
  start at the body with its argument unset. D-094 fixes the lifter.
