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
| [010](010-gta5-boot-8-mac.md) | 2026-10-03 | 0d93ebb | M4 Mac, pspocket-boot | Network up; first job chain attribute (14 arguments) |
| [011](011-gta5-boot-9-mac.md) | 2026-10-03 | 4f47fde | M4 Mac, pspocket-boot | Seven job chains and the `dependency_spu` taskset created; next gap `cellSpursEventFlagAttachLv2EventQueue` |
| [012](012-gta5-boot-10-mac.md) | 2026-10-03 | 1c8ec8f | M4 Mac, pspocket-boot | Event flag attached to its lv2 queue; next gap `cellSpursTaskGetLoadableSegmentPattern` (task creation) |
| [013](013-gta5-boot-11-mac.md) | 2026-10-04 | f8c82a4 | M4 Mac, pspocket-boot | Crash in `_cellSpursTaskAttributeInitialize` (a register holding 0x40 read as a pointer); crash reports now show the call site |
| [014](014-gta5-boot-12-mac.md) | 2026-10-04 | a45e338 | M4 Mac, pspocket-boot | Bink fixed; 4 dependency tasks created; first file access needs the disc |
| [015](015-gta5-boot-13-mac.md) | 2026-10-04 | ad558c6 | M4 Mac, pspocket-boot | Disc image copied from the iPhone and read in place; archives open; next gap `_cellGcmInitBody` |
| [016](016-gta5-boot-14-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | The RSX comes up (lv2 sys_rsx, cellGcmSys); next gap `cellVideoOutGetState` |
| [017](017-gta5-boot-15-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | 720p video output configured, Bink tasks created (libsre signature); first job chain "EDGE/GFX"; next gap `cellSpursRunJobChain` |
| [018](018-gta5-boot-16-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | Vblank handler called at 59.94 Hz; next gap `cellKbInit` |
| [019](019-gta5-boot-17-mac.md) | 2026-10-04 | ad558c6+ | M4 Mac, pspocket-boot | 60 s without a crash: input, tiles, first frame's job chain kicks; the game waits on SPU work |
| [020](020-gta5-boot-18-mac.md) | 2026-10-04 | 5cabf48+ | M4 Mac, pspocket-boot | SPU tasks run natively on SPURS workers; job chains and the custom workload don't yet |
| [021](021-gta5-boot-19-mac.md) | 2026-10-04 | 5cabf48+ | M4 Mac, pspocket-boot | GTA V's own SPU module runs, audio starts, no unimplemented calls; the game waits on its job chains |
