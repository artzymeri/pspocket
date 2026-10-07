# Runs 209–216 · A packet's arguments read in one run by the RSX (tried, not kept, D-229)

2026-10-07 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt` (128 s,
the 60-90 s windows); Assassin's Creed (BLES00158), `games/ac1/walk-training.txt` (600 s, the crowd windows: SPU threads
over 10.5 s per 5 s). Before and after alternating in one sitting, under caffeinate. Logs in inventory/.

## The change

The RSX's command reader took a packet's arguments one per pass of its loop: each word read with an acquire load,
recorded, dispatched, and the get pointer stored. The change read the words a packet still has (as far as put, the
end of the 1 MB segment and the pass's budget) in one inner loop, publishing get once at the end, and parked an acquire
that blocks inside the packet on its own word.

## The numbers

| | before | after |
|---|---|---|
| GTA V, the RSX thread, CPU s per 5 s (runs 209/211, 210/212) | 1.60, 1.79 | 1.69, 1.72 |
| GTA V, fps | 29.8, 29.9 | 29.8, 29.9 |
| Assassin's Creed's crowd, the RSX thread (runs 213/215, 214/216) | 3.16, 3.13 | 3.26, 3.06 |
| Assassin's Creed, process CPU s per 5 s | 19.34, 19.39 | 19.54, 18.07 |
| Assassin's Creed, fps | 31.9, 32.3 | 32.0, 33.1 |

No difference beyond the noise: the reader's per-word overhead is not where the RSX thread's time goes (D-210, D-211:
the draws). Reverted (CLAUDE.md rule 8). Its test stays: an acquire that blocks inside a packet must park on its own
word and run the rest of the packet after, whatever the reader.
