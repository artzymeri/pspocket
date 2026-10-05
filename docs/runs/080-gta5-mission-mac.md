# Run 080 · Further into the first mission, with the sticks

2026-10-05 · 3a3e3fb+ (D-108) · M4 Mac · `pspocket-boot ... --timeout 330 --frames DIR --frame-every 600 --rsx-recover
--flash inventory/firmware/dev_flash --spu-dump inventory/gta5/spu`, the intro presses of run 079, then walking
(`ls-up`, `ls-left`, `rs-right`), the phone (`up@148+6`, `cross@150+6`), shooting (`r2`) and aiming (`l2`).

| Round | SPU lift seeded from | SPU instructions interpreted | Images with no lifted program | fps |
|---|---|---|---|---|
| 1 | runs up to 079 | 586 million | 0 | 30 |
| 2 | + round 1 | 25.9 million (the phone was not reached) | 0 | 30 |
| 3 | same | 4,328 million: the charge goes off | 27 (saved) | 30, dips |
| 4 | + rounds 2 and 3, the 27 images | 648 million | 0 | 30 |
| 5 | + round 4 | 64.8 million | 0 | 30 |
| 6 | + round 5, `ila` offsets as entries (dropped) | 54,569 million | 3 | 10 |
| 7 | every log, lifted again from scratch | **0** | 0 | **30** |

- What the game does: Michael walks through the bank, the hint "Press up to bring up the phone" and "Use the phone to
  trigger the explosive charge" come up, the phone sets the charge off, and the alarm lights the vault corridor red.
  Shooting leaves bullet holes and counts the ammunition down.
- Round 6: data taken for code made jobs' own DMAs look like overwrites of their lifted code, so their programs were
  dropped and interpreted for the rest of the run (D-108).
- RSX: one failure in the run (MSAA, D-102); `NV4097_SET_BEGIN_END` primitive 36 seen once in round 2 (D-108). GPU
  16 ms a frame, 0 ms waiting for readbacks (D-107).
