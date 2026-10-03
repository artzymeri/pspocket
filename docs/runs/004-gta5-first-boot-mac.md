# Run 004 · GTA V's EBOOT boots under the interpreter on the Mac

2026-10-03 · build a92b4c2 · M4 Mac · `pspocket-boot <decrypted EBOOT.elf> --timeout 60` (no lifted code: every
guest instruction interpreted). Game files and the full log stay in `inventory/`; only API names appear here.

## What happened

| Step | Result |
|---|---|
| Load | 2 LOAD segments, 362 imported functions linked (1 implemented: `sys_initialize_tls`), 0 imported variables |
| TLS | `sys_initialize_tls` ran before the entry; TLS block 0x2CC bytes (0x2FC per thread, 343 threads in the area) |
| Entry | Main thread started at the CRT entry; the CRT called `sys_initialize_tls` again (no-op, as on RPCS3) |
| First gap | `sysPrxForUser::sys_lwmutex_create` returned CELL_ENOSYS |
| Then | Syscall 988 (debug PPU exception handler, `null_func` on RPCS3) → CELL_ENOSYS; the CRT called `abort()`, which printed its call chain through `sys_tty_write` |
| Exit path | `sysPrxForUser::sys_process_exit`, `sys_prx_exitspawn_with_level`, `sys_ppu_thread_get_id` unimplemented; execution ran past the non-returning exit and the host died with SIGBUS (an unreported guest fault) |

## What changed because of it (D-034)

- sysPrxForUser: `sys_lwmutex_create/destroy/lock/trylock/unlock`, `sys_ppu_thread_get_id`, `sys_process_exit`,
  `sys_prx_exitspawn_with_level`.
- lv2: syscall 988 returns CELL_OK like RPCS3's `null_func`.
- pspocket-boot: guest faults name the guest address and the PPU instruction address; `--imports` lists all imports
  with their implementation state; repeated calls to an unimplemented function no longer flood the log.

Next run: the same command, plus `--imports` once to record which of the 362 functions GTA V imports per module.
