# Run 036 · GTA V boot, round 34: a code module loaded from zeros

2026-10-04 · 68706ad+ · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-035.log).

| Step | Result |
|---|---|
| Report | With the SPU registers: the job's module loader (LS 0x4C30) fetched a 128-byte header whose address and size fields are 0, then the module from address 0 |
