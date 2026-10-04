# Run 023 · GTA V boot, round 21: the Secondary chain runs, then runs out of SPU memory

2026-10-04 · uncommitted tree on 437b72e · M4 Mac · `scripts/boot.sh` (inventory/boot-021.log).

| Step | Result |
|---|---|
| Event flag bits | The Secondary chain's job sets bit 0 of an lv2 event flag through its interrupt mailbox |
| Job chains | The Secondary chain gets through about 55 commands |
| End | Job 0x1D21600 (binary 48 pages, input 6, output 1, scratch + stack 177: 232 of the 234 heap pages) gets `CELL_SPURS_JOB_ERROR_MEMORY_SIZE`; the chain halts on errors: fatal |

The previous job's scratch + stack was freed only when this job's was allocated, after its other buffers: it is now
freed first when the sizes differ (D-055).
