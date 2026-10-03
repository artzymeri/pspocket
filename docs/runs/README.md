# Device and host runs

One file per device session (or host bring-up run): what was run, on what build, the numbers, and what they mean for the plan. Reports are
the raw text the test host writes (`scripts/ios.sh report`). No game data appears here.

| Run | Date | Build | Device | Result |
|---|---|---|---|---|
| [001](001-first-device-selftest.md) | 2026-10-03 | e557a8a | iPhone 17 Pro Max, iOS 26.6.1 | Self-test 6/6 pass; MetalFX 1152×640 at 30 Hz, 0 dropped |
| [002](002-thermal-10min.md) | 2026-10-03 | e557a8a | iPhone 17 Pro Max, iOS 26.6.1 | 10 min max GPU load: nominal thermals, GPU time flat, 0/18,241 dropped |
| [003](003-ppu-lifted-on-device.md) | 2026-10-03 | 2bef847 | iPhone 17 Pro Max, iOS 26.6.1 | 8/8; PPU interpreter 317 MIPS, lifted 12.0× faster (≈3,690 MIPS) |
| [004](004-gta5-first-boot-mac.md) | 2026-10-03 | a92b4c2 | M4 Mac, pspocket-boot | GTA V EBOOT boots under the interpreter; first gap `sys_lwmutex_create` |
| [005](005-gta5-boot-2-mac.md) | 2026-10-03 | 442ac11 | M4 Mac, pspocket-boot | Past the CRT; next gaps `sys_time_get_system_time`, `cellSysmodule` |
| [006](006-gta5-boot-4-mac.md) | 2026-10-03 | 8cf53e3 | M4 Mac, pspocket-boot | Two SPURS instances up natively; next gap `sys_vm_memory_map` |
| [007](007-gta5-boot-5-mac.md) | 2026-10-03 | db14915 | M4 Mac, pspocket-boot | 60 s of game code without a crash; tasksets, thread creation and PSN status requested |
| [008](008-gta5-boot-6-mac.md) | 2026-10-03 | 246075a | M4 Mac, pspocket-boot | Threads run; Bink tasksets; sheap, network and SPU setup requested |
| [009](009-gta5-boot-7-mac.md) | 2026-10-03 | d749522 | M4 Mac, pspocket-boot | SPU and network setup done; next gap `cellNetCtlInit`; Bink waits on native SPU tasks |
