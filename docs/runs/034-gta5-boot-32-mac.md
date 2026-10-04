# Run 034 · GTA V boot, round 32: atomic reservations; a fault outside the PPU

2026-10-04 · 68706ad+ (D-064, D-066) · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-033.log).

| Step | Result |
|---|---|
| Audio | The audio engine and mixer threads cycle normally (waits of microseconds); a "Bink Snd" thread starts |
| End | About 15 s: a guest access to address 0 by a thread that is not a PPU thread |
