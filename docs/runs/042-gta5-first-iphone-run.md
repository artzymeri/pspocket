# Run 042 · GTA V on the iPhone: the first device run

2026-10-04 · 0770578+ (D-071) · iPhone 17 Pro Max, iOS 26.6.1 · the app's Game tab: Choose ISO (Files), Boot, then
Cross on the screen's buttons at the calibration screen. The log (`Documents/game.log`) was followed from the Mac by
copying it every 5 s (`xcrun devicectl device copy from`).

| Step | Result |
|---|---|
| Boot | BLES01807 boots from the disc image picked in Files, read in place (17.45 GB); the executable and flashMP3.pic were copied from the Mac (`scripts/ios.sh push-game`) |
| Setup | The same SPURS instances and workloads as on the Mac; the trophy set registers (50 trophies); the MP3 codec loads from /dev_flash |
| Frames | The opening animation (its audio port closes at 18:45:26), the legal screen, Display Calibration, on the phone's screen |
| Input | Cross, pressed on screen, confirms calibration: the story save list (none) at 18:45:41, then the profile saved at 18:46:28 (`RAGE.SAV` and `ICON0.PNG` in `hdd0/home/00000001/savedata/BLES01807PROFILE`) |
| End | 18:47:05: `_cellSpursSemaphoreInitialize`, as run 041: "Loading Story Mode" can't finish yet |

The phone keeps the Mac's pace on this stretch (interpreters on both): calibration to the semaphore gap in about
1.5 minutes. No crash, no failed draws in the log.

**Frame rate** (a later boot of the same build, with the frame rate logged every 5 s): 6 to 9 fps during the opening
animation (a Bink video decoded on SPUs), then 29.7 to 29.9 fps on the loading screens, the game's own 30 fps cap.

**Where the phone's time goes** (Instruments Time Profiler, 10 s during the opening animation, recorded from the Mac
with `xcrun xctrace record --attach PSPocket`): all six cores are busy. The six SPURS workers take about 88% of the CPU
(each a full core); the PPU threads about 10%; the RSX about 1%. On four of the workers, 30 to 50% of the time is the
kernel's workload selection and another 25 to 35% policy overhead: polling, not game work (GTA V's own policy module
looking for work, D-064). The PPU interpreter spends about a quarter of its time decoding instructions. Two fixes for
when the game is playable and frame rates can be measured in the game itself: let idle SPU workers sleep instead of
spinning, and run translated code (PPU and SPU lifting) instead of the interpreters.
