# Run 009 · GTA V boot, round 7: past SPU and network setup, into cellNetCtl

2026-10-03 · build d749522 · M4 Mac · `scripts/boot.sh` (inventory/boot-007.log).

| Step | Result |
|---|---|
| Bink | Event flag initialized; then `_cellSpursTaskAttributeInitialize` (task creation) failed and Bink shut its taskset down cleanly, twice. Left unimplemented on purpose until SPU tasks can run natively (stage 3b) |
| Setup | `cellSheapInitialize` (3 arguments: r6 is not a DMA tag), `sys_spu_initialize(6, 0)`, `sys_net_initialize_network_ex` all succeed |
| Gap | `cellNetCtl::cellNetCtlInit`, then the game's assertion |

Changed because of it: cellNetCtl Init, Term, GetState (Disconnected), AddHandler, DelHandler; cellSheapInitialize's
signature.
