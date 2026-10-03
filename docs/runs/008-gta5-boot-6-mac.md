# Run 008 · GTA V boot, round 6: threads run, Bink tasksets, network and SPU setup

2026-10-03 · build 246075a · M4 Mac · `scripts/boot.sh` (inventory/boot-006.log).

| Step | Result |
|---|---|
| Threads | Thread creation works: two game threads started (interpreted) |
| SPURS | Both tasksets are Bink's ("RAD Game Tools Task"), workloads 0 and 1 of "Primary". Bink then called `_cellSpursEventFlagInitialize` (failed) and shut each taskset down with `cellSpursShutdownTaskset` |
| Then | `cellSheap::cellSheapInitialize` (512 KB), `_sys_process_atexitspawn`, `_sys_process_at_Exitspawn`, syscall 169 `sys_spu_initialize(6, 0)`, `sys_net::sys_net_initialize_network_ex`, then the game's assertion (`tw 31`) |

Changed because of it (D-039): SPURS event flag initialize and taskset/workload shutdown; cellSheapInitialize;
the two exitspawn registrations; `sys_net_initialize_network_ex` (CELL_OK, as RPCS3); `sys_spu_initialize`.
