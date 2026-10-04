# Run 024 · GTA V boot, round 22: MEMORY_SIZE again

2026-10-04 · uncommitted tree on 437b72e · M4 Mac · `scripts/boot.sh` (inventory/boot-022.log).

| Step | Result |
|---|---|
| Audio | `cellAudioAddData` for the first time: the Secondary chain is GTA V's audio mixer |
| End | The same job, 24 commands into the chain, gets `MEMORY_SIZE` again: fatal |

The error now reports which buffer had no room and how the pages are laid out.
