# Run 016 · GTA V boot, round 14: the RSX comes up

2026-10-04 · uncommitted tree on ad558c6 (GCM and sys_rsx, D-045) · M4 Mac · `scripts/boot.sh` (inventory/boot-014.log).

| Step | Result |
|---|---|
| `_cellGcmInitBody` | Context pointer, 3 MB of command buffer (96 segments of 32 KB), 192 MB of main memory at 0x10100000 for the RSX |
| lv2 | RSX local memory at 0xC0000000 (249 MB); device, DMA control, driver info and reports at 0x40000000..0x40300000; the RSX thread starts with the null backend |
| Mapping | `cellGcmMapEaIoAddressWithFlags`: 1 MB at 0x10000000 to IO 0x0E000000, the main-memory report area |
| Next gap | `cellVideoOutGetState` (unimplemented); the game asserts (`tw 31,r1,r1` at 0x00b1cdf8) |

Changed because of it: cellSysutil and cellSysutilAvconfExt (D-048).
