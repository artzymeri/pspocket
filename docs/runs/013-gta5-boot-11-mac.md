# Run 013 · GTA V boot, round 11: a bad pointer in task attribute initialize

2026-10-04 · build f8c82a4 · M4 Mac · `scripts/boot.sh` (inventory/boot-011.log).

| Step | Result |
|---|---|
| Bink | Taskset "RAD Game Tools Task" created; then `_cellSpursTaskAttributeInitialize` (attr 0xd0043830, called from 0x016756f8) |
| Crash | SIGBUS: guest access to 0x00000040 inside that function. It read its LS pattern or argument pointer (r9 or r10), and one of them held 0x40 |

RPCS3 leaves `_cellSpursTaskAttributeInitialize` unimplemented, and its default setup runs Sony's libsre instead of
its own SPURS code (D-042), so the signature was a guess (D-041). Changed because of it: the LS pattern is read only
when the task has a context, unreadable pointers are refused with `CELL_SPURS_TASK_ERROR_FAULT`, and both these
refusals and `pspocket-boot`'s crash report now show r3..r10 and disassemble the caller's last 16 instructions.
Every `_cellSpursTaskAttributeInitialize` call is logged with its arguments, so run 014 shows which register held
0x40 and how the game computed it.
