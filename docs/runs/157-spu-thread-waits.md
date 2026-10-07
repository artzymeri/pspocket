# Runs 157–171 · Where GTA V's SPU threads wait; the tracker's line scan in NEON (D-223)

2026-10-07 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt`, 128 s
(the measured windows 60-90 s); "under load": six threads spinning at the SPU threads' QoS class, standing in for the
iPhone's full cores. Profiles with macOS `sample`, 15 s at about 90 s. Logs in inventory/gta5.

## The profile (run 157, under load)

Of the five main SPU threads' busy samples (about 10,600 of 21,145; the rest sleeping), translated code took about 60%.
Of the translated code: shufb 10.7%, untagged lines (the C compiler's spills) 10%, lqx 7.7%, function entries 6.9%, fma
6.4%, brsl 6.2%, lqd 5.7%, exit stores 5.5%, stqd 4.2%. Outside it: yielding (swtch_pri) about 12% of the busy time,
memmove (DMA) 4%, the tracker's release 3%, ps_res_notify_write 2.4%.

The yields: waiting for the EDGE/GFX job chain's lock (552 samples) and re-reading contended lines in ps_res_acquire
(300+, mostly the SPURS kernel's workload selection on the SPURS line).

## Sleeping on the chain's lock (runs 158-166, tried)

| run | | main SPU threads, CPU s per 5 s | fps |
|---|---|---|---|
| 158, 160 | before, idle | 6.79, 6.64 | 29.9 |
| 159, 161 | lock wait sleeps, idle | 6.64, 6.65 | 29.9 |
| 162, 164 | before, under load | 9.30, 9.03 | 28.4, 28.0 |
| 163, 165 | lock wait sleeps, under load | 9.85, 9.55 | 29.0, 28.5 |

Run 166's profile (under load, the change in): the chain-lock yields gone, the select1 yields 252 to 535. The waiting
moved to the SPURS line; reverted.

## The line scan in NEON (runs 167-171, D-223)

| run | | main SPU threads | audio SPU | process |
|---|---|---|---|---|
| 167 | before | 6.67 | 1.43 | 11.39 |
| 168 | NEON scan | 6.40 | 1.37 | 11.01 |
| 169 | before | 6.45 | 1.39 | 11.06 |
| 170 | NEON scan | 6.40 | 1.35 | 10.96 |

About 2.5% less; release's samples 311 to 100 (run 171). Modules lifted 708,000 to 720,000 in every run, none
interpreted. The gate passes.
