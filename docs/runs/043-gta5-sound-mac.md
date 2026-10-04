# Run 043 · GTA V's sound on the Mac: whole blocks, and how fast the game makes them

2026-10-04 · 7ea8393+ (D-074) · M4 Mac, `pspocket-boot --audio --timeout 90..100` with the disc image and /dev_flash, four
runs. The reason: on the iPhone (run 042's build) the sound buzzed "like bad radio" on the loading screen.

cellAudio played a period every 5.33 ms by the clock and never looked at the blocks. GTA V adds each block with
`cellAudioAddData` (to the block at the read index) once per mix event, so a late answer lands on the next block: a
silent block, then possibly a doubled one. The four runs try RPCS3's buffering mode, which waits briefly for the game
before playing a block, with the sound line D-074 adds every 5 s.

| Log | Waiting for the game | Result |
|---|---|---|
| `boot-042.log` | RPCS3's: a count of ports given up on, 2 periods | Whole blocks for 50 s; then the count stuck at one when GTA V closed its unused second port, and half the periods played silent in a row (the buzz). 86.2 s played, 21 underruns: the silence filled the gaps |
| `boot-043.log` | per port (fed in the last 340 ms), 8 periods | No silent periods; 53.4 s played, 492 underruns |
| `boot-044-wait2.log` | per port, 2 periods (RPCS3's) | No silent periods; 52.4 s played, 501 underruns |
| `boot-045-wait0.log` | per port, none | Some silent blocks among the game's while its second port played; 64.1 s played, 318 underruns |

In all four the game made about the same audio: about 9,500 `cellAudioAddData` blocks in 100 s, all of real time while
the opening animation plays, 55 to 65% of it on the loading screens and 20% in the heaviest stretch (after flip 1024,
17,000 draws per 1,000 flips). No run had a doubled block worth counting (0 to 3 per 5 s). So the wait only decides how
often a period goes on without the game; D-074 keeps RPCS3's limits with per-port tracking (`boot-044`'s setup).

What's left is the game itself: its audio thread and the SPU mixer run under the interpreters, and the speaker runs dry
when they can't keep up, so the sound comes in bursts with gaps instead of buzzing. The next steps are time stretching
(slower but continuous sound while the game is behind, as RPCS3's option and the ps3-ios-engine port do) and, for real,
translated SPU and PPU code.
