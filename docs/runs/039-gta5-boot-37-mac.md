# Run 039 · GTA V boot, round 37: no crash; a grey screen

2026-10-04 · 68706ad+ (D-064, D-066, D-067) · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-038.log).

| Step | Result |
|---|---|
| /dev_flash | The game opens and loads flashMP3.pic; the mixer runs |
| End | The 120 s timeout, no crash. Every thread is in a steady loop |
| Frames | 2,670 flips: the opening animation, black, then grey |
| RSX | 110,146 draws skipped, all on six fragment programs that update condition codes |
