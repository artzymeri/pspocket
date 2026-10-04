# Run 032 · GTA V boot, round 30: the audio engine runs; a job slot used twice

2026-10-04 · 68706ad+ (D-064) · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-031.log).

| Step | Result |
|---|---|
| Scheduler | With priorities right (D-064) the AudioEngine chain runs; workload creation logs show each workload's priorities |
| End | Early, before the Physics taskset: the Secondary chain (the audio mixer) fetches a cache element of 0xE0 bytes at address 0x140, an MFC fault on the PS3 (fatal log) |
