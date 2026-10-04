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
