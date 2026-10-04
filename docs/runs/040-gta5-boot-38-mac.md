# Run 040 · GTA V boot, round 38: Display Calibration

2026-10-04 · c6b357d+ (D-067, D-068) · M4 Mac · `pspocket-boot ... --timeout 150 --flash inventory/firmware/dev_flash --frames ... --frame-every 30` (inventory/boot-039.log).

| Step | Result |
|---|---|
| RSX | Fragment program condition codes (D-068): 3,656 flips, **no failed draws** |
| Frames | The opening animation, the legal screen (licence and copyright text), then **GTA V's Display Calibration screen**: the Rockstar logo, the brightness bar, the text and the "Adjust / Confirm" prompts |
| End | The 150 s timeout, no crash: the game waits for a button press on the calibration screen |

Next: a way to press the virtual controller's buttons during a run, so the game can go past this screen.
