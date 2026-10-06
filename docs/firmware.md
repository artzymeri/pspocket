# The PS3 system software in pspocket

The PS3's system software comes as a single update file, PS3UPDAT.PUP. Users download it from Sony for free, and the
app installs it from Settings (D-156). It is Sony's closed software under Sony's licence, not open source. pspocket
reads the user's own copy on the user's own device so that games can work with it. Nothing from it ever goes into
this repository: no modules, no code, no fonts, no lists taken from it (rule 1). This page is the committed summary of
the analysis (D-176). Regenerate the full report (gitignored) with:

```sh
python3 scripts/firmware-report.py --rpcs3 RPCS3_DIR    # -> inventory/firmware/analysis/report.md
```

The report decrypts every module (`pspocket-firmware --decrypt`) and inspects it (`ps3inspect`). It reads what the
host implements (`pspocket-boot --hle-list`) and RPCS3's default for each module. Then it sorts every part into one
of the plans below.

## What version 4.92 holds

| Folder | Files | MB | What it is |
|---|---|---|---|
| sys/external | 144 | 17 | 142 system library modules (.sprx); flashATRAC.pic and flashMP3.pic, SPU decoders games load themselves |
| sys/internal | 38 | 2 | 37 more modules: codec cores, file utilities, the audio and voice services |
| data/font | 35 | 24.4 | The PS3's system fonts (cellFont's system font sets) |
| data/cert | 60 | 0.1 | Root certificates for SSL/HTTPS |
| data/dic | 28 | 7.0 | On-screen keyboard dictionaries |
| vsh | 673 | 102.5 | The XMB menu and its resources |
| bdplayer, ps1emu, ps2emu, pspemu | 97 | 34 | Blu-ray player and the PS1, PS2 and PSP emulators |

Across the 179 modules there are 206 libraries and 5,385 exported functions. The host implements 607 of those
functions today.

Inside the modules are 188 SPU programs (17.5 MB): the decoders and helpers that run on the PS3's SPUs.

## How each part meets the engine

Each part gets one of five plans. Function counts are what the modules export; the third number is what the host
implements today.

- **Ours on iOS** (77 modules, 2,814 functions, 598 implemented): the parts pspocket must own, because they talk to
  the PS3's hardware or its menu system. These are the process runtime, SPURS (D-037), graphics (RSX to Metal), sound
  (AudioUnit), controllers (GameController), files, network, clock, accessories, and sysutil with everything built on
  it (dialogs, saves, game data, trophies, PSN). Their modules never run as they are (D-175).
- **iOS framework** (24 modules, 347 functions): the same job done by iOS in hardware or tuned code.
  - Image decoders and encoders: ImageIO.
  - Video decoder cores: VideoToolbox for H.264. MPEG-2, MPEG-4, VC-1 and DivX stay Sony's code, translated.
  - Video and audio post-processing: Metal compute or vImage; AudioConverter for resampling.
- **Sony's code, translated once** (75 modules, 2,186 functions): work with no iOS equivalent, or work whose output
  must match the PS3 exactly.
  - Media: the cellAdec and cellVdec front ends, the audio decoder cores (ATRAC3, ATRAC3plus, WMA, DTS, TrueHD,
    MPEG audio, CELP; AAC, MP3 and AC-3 could move to AudioToolbox later), the ATRAC players, and the movie
    containers and players (PAMF, Dmux, Sail, MP4).
  - Text: fonts with FreeType, text conversion (cellL10n), key codes.
  - Threads: cellSync2, fibers, the SPURS job queue.
  - Network: HTTP, SSL and reliable UDP.
  - Also the resolution scaler (cellResc), the encoders and in-game ads.

  These run as they are on the engine's own lv2, SPURS and iOS-native services (D-175). Today they run under the
  interpreter (D-157). Translating them once per firmware, the same way a game is translated, makes them native code.
- **Data, kept**: the fonts and certificates, read as they are by Sony's translated code.
- **Not used** (crash dumps, debugger, boot OSD, vsh, bdplayer, the emulators, keyboard dictionaries): of no use to a
  game on an iPhone.

RPCS3, for comparison, runs 78 of these modules as they are by default and replaces 66 with its own code.

## Findings

1. **Codec cores also live in sys/internal.**
   - cellAdec (libadec) links to 22 codec cores. Five exist only in sys/internal: DTS-HD, AAC (MPEG-2), AAC 2ch, MP3
     surround and TrueHD.
   - Four exist in no module of the retail firmware: WMA Pro, WMA Lossless, DTS and DTS-HD core.
   - A PS3 leaves imports like these unlinked, and libadec decodes every other format.
   - D-175 indexed sys/external only and refused a module with an unlinked variable, so cellAdec stayed on the host,
     which has none of it. Fixed in D-177.
2. **The heavy work runs on the SPUs.** Video decoding (the H.264 decoder alone has 13 SPU programs), audio decoding,
   image decoding and video post-processing all run SPU programs through SPURS, which is ours. Making these modules
   native therefore means translating their SPU programs too, the way a game's are (D-117).
3. **Fonts come from the system file.** libfont and FreeType read the 35 system fonts through sys_fs from
   /dev_flash/data/font. A game that draws text with cellFont's system fonts needs them on the phone.
4. **Most of the installed files are never used on the phone.**
   - vsh, bdplayer, the PS1/PS2/PSP emulators and the keyboard dictionaries take 150 of the 188 MB.
   - What games use is sys (19 MB), data/font (24 MB) and data/cert.
   - The phone now keeps only those (D-177).
5. **Some imports never resolve on a real PS3 either.** These are cellLibprof (a profiler), sys_libc and
   sys_libstdcxx (in one module each), plus the four codec cores above. They are listed as gaps, not errors.

## Plan, in order

1. **D-177: link like a PS3, keep only what games use.** Index sys/internal as well; unlinked imports become gaps
   instead of refusals; the phone keeps only sys, data/font and data/cert.
2. **Translate Sony's code once per firmware** ("Preparing system…", shared by every game). Done for the PPU code
   (D-178): 100 modules at fixed addresses in the PRX region, 18,962 functions, an 18.8 MB system library made in 33
   s on the Mac. Each module is checked against its translation when it loads. Their SPU programs still come
   through each game's run-time discovery (D-117).
3. **iOS frameworks for heavy media, behind the same interfaces.** ImageIO for PNG, JPEG and GIF; VideoToolbox for
   H.264 behind cellVdec; AudioToolbox for AAC, MP3 and AC-3 behind cellAdec. Each is measured against Sony's
   translated version first (rule 8).
4. **Keep growing the parts that are ours** (sysutil, SPURS, input, audio, network, RSX). Each is filled in from what
   games call, with the gaps logged by D-175's choice.
