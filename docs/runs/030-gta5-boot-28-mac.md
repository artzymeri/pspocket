# Run 030 · GTA V boot, round 28: a CellSpursTaskset2, then SPURS queues

2026-10-04 · uncommitted tree on c68e12c · M4 Mac · `scripts/boot.sh --frames ...` (inventory/boot-029.log).

| Step | Result |
|---|---|
| Taskset2 | `cellSpursTasksetAttributeSetTasksetSize(10496)` and a CellSpursTaskset2 created (D-058) |
| Gaps | As run 029, then `cellSysutilEnableBgmPlayback`, `cellSysutilGetBgmPlaybackStatus`, `cellVoiceInit`, `_cellSpursQueueInitialize` |
| End | The game's assert (`tw 31,r1,r1` at 0x00BD2674) after `_cellSpursQueueInitialize` failed |

SPURS queues are next (RPCS3 has none: libsre is the reference), with save data and the smaller gaps alongside.
