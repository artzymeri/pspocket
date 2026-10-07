# Runs 150–156 · Job descriptors after the grab (D-222); the sound's pace under load (tried, not kept)

2026-10-07 · pspocket-boot with the Metal backend · M4 Mac · GTA V (BLES01807), `games/gta5/walk-prologue.txt`; runs
150 and 153-156 with `--audio` (the buffered sound path the iPhone uses, with its "sound:" lines every 5 s). Logs in
inventory/gta5 (kept out of the repository).

## Do job descriptors change after the chain takes the job? (runs 150-152)

GTA V's EDGE/GFX job chain stops now and then on a descriptor that holds the game's own RSX commands (improve.md,
Stability). We read a descriptor when its job starts, later than libsre's pipeline does, so a game reusing descriptors
once the chain has taken them would hand us new contents. D-222 keeps a hash and the header of each descriptor at the
grab and compares at the start.

| run | what the check hashed | length | descriptors changed between grab and start |
|---|---|---|---|
| 150 | all 0x280 bytes the chain reads per descriptor | 300 s | 28,518 |
| 151 | the same, with the changed words and the SPU writes logged | 110 s | every change at +0x210 or later, written by EDGE jobs (binary 0x1916180) 0.03-0.08 ms after the grab |
| 152 | what the job uses: header, input DMA list, cache list | 300 s | **0** |

GTA V's descriptors are 0x200 apart and the chain reads 0x280: the last 0x80 bytes are other jobs' output, written
while the job waits. Nothing a job uses changes after the grab, so the late read is not what stops the chain in normal
play. The stop's report now starts with the descriptor as it was at the grab, for the next time one is caught.

## The sound's pace when the cores are full (runs 153-156)

On the iPhone the sound slows to 0.5-0.8 of its pace when the cores are full. Ten threads spinning at the SPU threads'
QoS class on the Mac reproduce it (the game at 24-25 fps). The change tried: the host threads that feed the sound are
found by what they write (cellAudio's port buffers and the samples cellAudioAdd*Data copies, through the SPU DMA write
path; the PPU thread calling cellAudioAddData) and get cellAudio's own QoS. It found GTA V's two at once (the mixer's
SPU thread writing 0x10c51000, the PPU thread adding those samples).

| run | feeders raised | pace mean / lowest | periods skipped | waits for the game | fps | mixer SPU, s per 5 s |
|---|---|---|---|---|---|---|
| 153 | no | 0.81 / 0.45 | 1,499 | 2,425 | 25.1 | 1.87 |
| 154 | yes | 0.80 / 0.15 | 1,434 | 2,539 | 24.1 | 1.93 |
| 155 | no | 0.85 / 0.57 | 1,226 | 2,396 | 25.1 | 2.07 |
| 156 | yes | 0.88 / 0.10 | 1,064 | 1,888 | 24.4 | 2.07 |

No difference beyond the noise: the mixer's thread had the CPU it wanted either way. The pace follows the game's frame
loop and main SPU workloads, upstream of the mixer. Reverted (rule 8); improve.md keeps the idea for a phone session,
where the mixer may sit on an efficiency core. For comparison, run 150 without the load: pace 1.06, the game at 29.8 fps.
