# Run 006 · GTA V boot, round 4: two SPURS instances up, then virtual memory

2026-10-03 · build 8cf53e3 · M4 Mac · `scripts/boot.sh` (log inventory/boot-004.log).

| Step | Result |
|---|---|
| Modules | NET, VOICE, RTC, SYSUTIL_NP_TUS, SYSUTIL_GAME loaded (bookkeeping, D-035) |
| SPURS | Two instances initialized natively (D-037): "Primary", 5 SPUs, priority 200, exclusive non-context group type; "Secondary", 1 SPU, priority 201. No tasks created yet |
| Gap | Syscall 300 `sys_vm_memory_map`: 32 MB virtual, 0x1260000 (18.4 MB) physical, default container, 64 KB pages. CELL_ENOSYS, then the game's assertion (`tw 31`) |

Changed because of it (D-038): `sys_vm_*` (syscalls 300–313) as RPCS3 models them.
