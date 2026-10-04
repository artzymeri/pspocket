# Run 025 · GTA V boot, round 23: why the job doesn't fit

2026-10-04 · uncommitted tree on 437b72e · M4 Mac · `scripts/boot.sh` (inventory/boot-023.log).

| Step | Result |
|---|---|
| Job chains | The Secondary chain gets through about 395 commands |
| End | Job 0x1D21E00 (binary 40 pages, input 6, output 1, scratch + stack 185) has no room for its scratch + stack: its binary went in at pages 67..106, after another job's cached binary at 19..66, and no 185 pages are left in one piece once that one is dropped. Fatal |

Jobs that fit only when packed are now laid out again in an emptied heap (D-055).
