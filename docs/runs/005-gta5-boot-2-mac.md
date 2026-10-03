# Run 005 · GTA V boot, round 2: past the CRT's lwmutex, into module loading

2026-10-03 · build 442ac11 · M4 Mac · `pspocket-boot <decrypted EBOOT.elf> --timeout 60` and `--imports`.

## Imports by module (`--imports`)

362 functions in 23 modules, 9 implemented at this build: cellSysmodule 4, cellVoice 14, sysPrxForUser 15 (9),
cellSheap 3, cellHttp 6, cellSsl 3, sceNpCommerce2 30, cellSpurs 43, cellNetCtl 9, sys_net 26, sys_fs 20, cellRtc 7,
cellSysutil 32, sceNp 51, cellGame 6, cellSaveData 1, cellGcmSys 20, cellSysutilAvconfExt 2, sys_io 14, cellAudio 11,
sceNp2 27, sceNpSns 6, sceNpTrophy 12. Matches the Phase 0 inventory (docs/phase0-gta5.md).

## What happened

| Step | Result |
|---|---|
| CRT | `sys_initialize_tls`, lwmutex creation and syscall 988 all succeed now; no `abort()` |
| Gaps, in order | `sysPrxForUser::sys_time_get_system_time`, `cellSysmodule::cellSysmoduleIsLoaded(NET)`, `cellSysmoduleLoadModule(NET)` |
| Then | The game used the CELL_ENOSYS result as a pointer: guest load from address 0, reported by the new fault handler with the PPU address (no more bare SIGBUS) |

## What changed because of it (D-035)

`sysPrxForUser::sys_time_get_system_time`; `cellSysmodule` Initialize / Finalize / LoadModule / UnloadModule /
IsLoaded as load-count bookkeeping over RPCS3's module table.
