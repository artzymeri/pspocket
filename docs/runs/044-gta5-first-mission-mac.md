# Run 044 · GTA V's first mission starts on the Mac

2026-10-04 · c503afa · M4 Mac · `pspocket-boot ... --timeout 1500 --flash inventory/firmware/dev_flash --audio
--frames ... --frame-every 300 --press cross@45 --press cross@55 --press cross@65 --press cross@75 --press cross@90`
(inventory/boot-046.log), stopped by hand after about 12 minutes.

| Step | Result |
|---|---|
| Boot to calibration | As run 041; Cross at 45 s confirms Display Calibration, the profile is loaded and saved |
| Loading Story Mode | Goes on past run 041's gap: the Physics taskset's SPURS semaphores (D-070) work |
| Prologue | **The first mission starts**: "Ludendorff, North Yankton, nine years ago.", the objective "Go to the guard.", the radar and its help text ("The Radar shows your position within the world."), frame 3000 |
| Picture | The HUD and text draw; the 3D world is black |
| RSX | New in the world's shaders and render targets: vertex program condition codes (SLT/SGT/SEQ/MOV/ADD into CC0, 25 programs), fragment DIVSQ (19), vertex TXL (vertex texture fetch, 3), fragment TXL on a cube map, RET and PK16 (1 each); X16 textures (29 textures), Y16_X16, X32_FLOAT and W16_Z16_Y16_X16_FLOAT sampled from render targets, GPU readback of a float16 surface; multisampled surfaces, occlusion queries and conditional rendering are approximated |
| Sound | In the mission the game delivers its audio at 17 to 30% of real time (the SPU and PPU interpreters) |
| Log | 80,000 lines in the mission's first minutes: every SPURS task start, task creation and semaphore setup was logged (thinned in D-075) |

Next: the shader and texture gaps above, so the world renders, then the frame rate in the mission.
