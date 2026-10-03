# Phase 0 inventory: Grand Theft Auto V (BLES01807)

2026-10-03 · aggregate numbers only; the raw outputs (`ps3inspect` text and JSON, decrypted executables, extracted
SPU images, lifter logs) stay in `inventory/gta5/` (gitignored, D-021).

## How it was made

1. The user's disc image (18.74 GB, decrypted at disc level) sits on the iPhone in the app's Documents. The app's
   `-extract-iso` mode read it in place with the new ISO 9660 reader and copied out only PS3_DISC.SFB, PARAM.SFO and
   EBOOT.BIN (14.6 MB). GTA V ships no PRX modules on the disc.
2. The EBOOT (a disc SELF, no NPDRM header, key revision 0x1c) was decrypted headless on the Mac with the official
   RPCS3 build v0.0.43-20193 (`rpcs3 --decrypt`), giving a 28.6 MB ELF. No firmware or licence was needed.
3. `ps3inspect` produced the inventory; `ppu-lift` lifted the whole PPU code once to measure coverage (in 9.8 s,
   2.3 GB peak RAM, 1.3 GB of C in 64 translation units; the output was deleted afterwards).

Hashes are in `games/gta5/manifest.toml`.

## Executable

| | |
|---|---|
| Type | PPU executable (ELF64 big-endian, PPC64), entry descriptor 0x01A70A08 → code 0x000109E4, TOC 0x01ADF8A8 |
| SDK | 4.30; primary thread priority 2048, stack 256 KB; malloc page size 1 MB; TLS 716 bytes |
| Code segment | 27.6 MB (R-X at 0x00010000); `.text` 23.4 MB |
| Data segment | 0.9 MB file, 6.2 MB in memory (RW- at 0x01A70000) |
| Function descriptors | 52,704 (36,560 distinct functions) |
| Functions found by the analyzer | 85,342 (descriptors plus call targets) |
| PPU instructions | 6,086,213 |

## PPU instruction coverage (Phase 1 gate: "the ELF lifts with no unsupported instructions")

- Every non-VMX instruction GTA V uses is supported today: **0 unsupported scalar instructions, 0 illegal words**.
- **VMX: 448,215 instructions (7.4%), 98 distinct mnemonics.** Most frequent: lvx 93k, vmaddfp 64k, stvx 63k,
  vspltw 59k, lvlx 22k (Cell-specific), vspltisw 20k, vperm 18k, vsldoi 17k, vsubfp 12k, vaddfp 12k, vnmsubfp 7k,
  vcfsx 6k, vslw 5k, vand 5k, vsel 4k, vmrghw 4k, stvewx 3k, vrsqrtefp 3k, vcmpgefp. 3k. VMX is being implemented
  now; the gate is met when it lands.
- **Jump tables: 0 recognised, 6 indirect jumps.** The analyzer knows GCC's switch patterns; GTA V was most likely
  built with SN Systems' SNC, whose switch lowering differs (or is mostly compare chains). To investigate before
  Phase 2: every unrecognised `bctr` becomes a runtime dispatch, which is correct but slower.

## Firmware imports: 362 functions in 23 modules, all named

| Group | Modules (functions) | Plan |
|---|---|---|
| Graphics | cellGcmSys (20) | HLE, with the RSX FIFO (Phase 4) |
| SPU scheduling | cellSpurs (43): tasksets, job chains, queues, event flags, semaphores, barriers, workloads | see SPURS below |
| System | cellSysutil (32), cellSysutilAvconfExt (2), cellSysmodule (4), cellGame (6), cellSaveData (1), cellRtc (7), cellSheap (3) | HLE |
| Kernel-side libraries | sysPrxForUser (15), sys_fs (20) | HLE over lv2 (in progress) |
| Input, audio | sys_io (14: pads), cellAudio (11) | HLE (Phase 5) |
| Online, PSN, trophies, voice | sceNp (51), sceNp2 (27), sceNpCommerce2 (30), sceNpSns (6), sceNpTrophy (12), cellHttp (6), cellSsl (3), cellNetCtl (9), sys_net (26), cellVoice (14) | **184 of 362: non-goals.** Stubs that report "offline / not signed in" cleanly, so single-player never waits on PSN |

So 178 firmware functions need real implementations, plus whatever SPURS needs internally.

## SPU programs

8 SPU ELF images embedded in the EBOOT, 494 KB in total, all SPURS tasks:

| Program | Size | Class |
|---|---|---|
| inflateServer_task | 26 KB | zlib inflate (third-party middleware; replacement candidate) |
| dependency_spu_task | 14 KB | custom |
| collision_task | 135 KB | custom (physics) |
| forcesolver_task | 34 KB | custom (physics) |
| forcesolverpush_task | 34 KB | custom (physics) |
| updatecolliders_task | 103 KB | custom (physics) |
| updatecontacts_task | 44 KB | custom (physics) |
| binkspu_task | 105 KB | Bink video (RAD Game Tools) |

Not visible statically: **SPURS job chains.** GTA V creates job chains (`cellSpursCreateJobChainWithAttribute`,
`cellSpursRunJobChain`), and job binaries are not ELF images, so they only show up in the runtime trace (Phase 0 step
3, still to do). The SPURS kernel and its taskset/job-chain policy modules are Sony code from the firmware (libsre).

## Middleware found by signature

RAGE (Rockstar's engine), Euphoria / NaturalMotion (61 hits), Scaleform GFx (71 hits; the UI), Bink (video, with its
SPU task), zlib, an MP3 decoder, SPURS, NP/trophies.

## Go / no-go

**Go**, with three named risks that decide the outcome later:

1. **SPU performance (the decisive one).** GTA V keeps the SPUs busy (physics tasks, Bink, inflate, job chains).
   The phone has 2 performance and 4 efficiency cores for the PPU threads, the RSX thread and up to 6 SPUs' worth of
   work. Lifted PPU code already runs at a useful rate (run 003: ≈3.7 G instructions/s-equivalent per core on an
   integer benchmark), but SPU throughput is unmeasured. Checkpoint: the first lifted physics task timed on the
   device against its share of a 33 ms frame.
2. **SPURS.** 43 functions including job chains. Recommendation: LLE — recompile Sony's libsre (PPU and SPU parts)
   like game code, from a firmware file the user supplies (Sony publishes PS3UPDAT.PUP; it never enters the repo),
   because RPCS3's experience is that HLE SPURS is incomplete for job chains. Needs the user's firmware file.
3. **Code discovery.** Job binaries and any SPU overlays loaded at runtime need the RPCS3 trace (step 3). That trace
   runs the whole game in RPCS3 on the Mac and needs the 18.7 GB image there (17 GB free today) plus PS3 firmware
   installed in RPCS3.

## Re-estimate

The plan's estimate (1.5–3 years, part-time, GTA IV) assumed one engineer. With parallel agents, Phase 1 is close to
its gate within the first day (PPU interpreter, lifter and fuzzing done; VMX in progress), and the Phase 2–4
foundations are being built in parallel (D-018). The remaining time is dominated by GTA V-specific bring-up
(Phase 5: boot → menu → playable, driven by logs) and by the SPU-performance risk above, which nobody can estimate
before the first lifted SPU task runs on the phone. Honest range: **months, not weeks**, to a first boot to the menu;
playable at 30 fps is gated by risk 1.

## Next steps that need the user

- Confirm GTA V as the first title (D-025).
- For SPURS LLE: Sony's PS3 firmware file (PS3UPDAT.PUP), placed where the toolchain can read it (not in the repo).
- For the runtime trace: ~20 GB free on the Mac (or an external drive) for the image, and RPCS3 with that firmware.
