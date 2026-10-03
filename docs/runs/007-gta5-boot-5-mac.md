# Run 007 · GTA V boot, round 5: a full minute of game code, tasksets and threads requested

2026-10-03 · build db14915 · M4 Mac · `scripts/boot.sh` (inventory/boot-005.log). No crash: the run hit the 60 s
timeout with the game still running.

| Step | Result |
|---|---|
| Virtual memory | `sys_vm_memory_map` → 0x20000000, 32 MB virtual / 18.4 MB physical (D-038) |
| SPURS | Two tasksets requested on "Primary" (`_cellSpursTasksetAttributeInitialize`, `cellSpursTasksetAttributeSetName`, `cellSpursCreateTasksetWithAttribute`) |
| PSN | `sceNpManagerGetStatus` (non-goal: report not initialized / offline) |
| Threads | `sysPrxForUser::sys_ppu_thread_create` refused; the game printed "THREAD CREATION FAILED ([RAGE] netThreadPool 1)" and retried 237 times until the timeout |

Changed because of it: `sys_ppu_thread_create` / `sys_ppu_thread_exit` (TLS per thread, kernel create + start);
`sceNpManagerGetStatus`; native SPURS stage 3a (workload registration in the guest CellSpurs, taskset attributes and
creation; tasks themselves don't run yet).
