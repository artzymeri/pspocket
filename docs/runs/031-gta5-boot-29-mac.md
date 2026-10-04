# Run 031 · GTA V boot, round 29: SPURS queues; the intro animation; the audio engine starved

2026-10-04 · 68706ad · M4 Mac · `pspocket-boot ... --timeout 120 --frames ... --frame-every 30` (inventory/boot-030.log).

| Step | Result |
|---|---|
| SPURS queue | `_cellSpursQueueInitialize` (taskset "Physics", 128 entries of 16 bytes, SPU2PPU) and `cellSpursQueueAttachLv2EventQueue` (SPU port 0x13) succeed (D-063) |
| Frames | GTA V's opening animation (blurred lights, the wanted-stars spinner) over frames 30 to 270, then black, then flat grey from frame 750 |
| RSX | 1,125 flips; 32,259 draws skipped: fragment programs that update condition codes (`MOVR RC0, ...`) |
| Gaps | `sceNpTrophyGetRequiredDiskSpace`, `sceNpTrophyRegisterContext` (being written) |
| End | 72 s: SIGBUS in "[RAGE Audio] Mix thread" at 0x00B96FD0, a voice's virtual call through a null method |

**Why.** The audio engine thread had waited 64 s for job 24 of the "AudioEngine" job chain (priority 4), which never
ran: the scheduler weighed priorities upside down, so GTA V's own policy module (priority 12) and the Bink tasksets
always won the SPUs (fixed in D-064). The mixer ran on with voices the engine had not set up.
