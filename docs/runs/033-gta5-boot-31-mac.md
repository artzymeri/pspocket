# Run 033 · GTA V boot, round 31: one descriptor, two jobs

2026-10-04 · 68706ad+ (D-064) · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-032.log).

| Step | Result |
|---|---|
| Diagnosis | Job chain fatal logs now dump the job's descriptor. Both chains fault on the same descriptor, 0x01D22100: the mixer's binary and DMA list with the audio engine's argument and cache list. Two PPU threads had taken job slot 11 from the audio system's lock-free stack at once |
| Cause | PPU `stwcx.` was still the single-thread placeholder: compare, then store, by value only, so two threads could both succeed (fixed in D-066) |
