# Run 035 · GTA V boot, round 33: the fault is the mixer's SPU job

2026-10-04 · 68706ad+ · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-034.log).

| Step | Result |
|---|---|
| Report | Fault reports now name the host thread and the SPU code (D-067): "SPURS 10016000 SPU 0" (the Secondary chain, the audio mixer), `wrch MFC_Cmd` at LS 0x04D34, a 0x80-byte GET from EA 0 |
