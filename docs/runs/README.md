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
