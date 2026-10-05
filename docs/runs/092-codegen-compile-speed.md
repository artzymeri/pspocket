# Run 092 · Route B, stage 2: how fast the device's compiler turns GTA V's translated C into code (D-151)

2026-10-05 · working tree on 05c0dff (runtime/codegen, the codegen.compile probe, pspocket-cc) · M4 Mac (4 performance
and 6 efficiency cores, 16 GB), then the iPhone 17 Pro Max.

The compiler is the one the phone runs: LLVM 22.1.8's clang front end, AArch64 back end and lld's Mach-O linker as
static libraries (`scripts/build-llvm-ios.sh`), called in process (`runtime/codegen`), against clang's own headers and
`runtime/codegen/sysroot` (no SDK), with the flags of the build (-O2, `-ffp-contract=off -fno-fast-math`). GTA V's
translated code is the gate build's (`inventory/gta5`, never committed).

## One file at a time (M4, one performance core; `scripts/codegen-bench.sh --mac`)

| File | C | -O1 | -O2 | Peak memory |
|---|---|---|---|---|
| PPU unit game_ppu_100.c | 1.8 MB | 1.9-2.1 s | 1.9-2.2 s | +195 MB |
| SPU program 2607cab9 | 2.1 MB | 40-47 s | 40-46 s | +520 MB |
| SPU program bb7fe62f (the biggest) | 7.6 MB | 32-35 s | 35-38 s | +620-900 MB |
| PPU unit on 2 / 4 / 6 threads at once | 1.8 MB each | | 2.1 / 2.2 / 3.0 s | +180 / +380 / +580 MB |

-O1 costs as much as -O2 and makes no smaller objects, so -O2 stays. Apple's clang 17 takes 31.4 s and 549 MB for the
same 2.1 MB SPU program (ours 1.4x slower: Apple builds clang with PGO and LTO, ours is a plain release build).

## The whole game (`pspocket-cc -j 10 -O2`, largest file first)

| | Files | C | Wall | Thread time | Peak memory | Slowest file |
|---|---|---|---|---|---|---|
| PPU | 513 | 823 MB | 258 s | 2,567 s | 2.0 GB | game_ppu_234.c (one function of 12,919 lines): 119 s |
| SPU | 300 (150 programs) | 86 MB | 232 s | 1,864 s | 5.6 GB | spu_8ce4b994 (6.5 MB): 174 s |

About 8 minutes for all of GTA V on the M4. Where the time goes:
- **LLVM's register allocator on giant functions.** In the 2.1 MB SPU program 18 of 31 s are the greedy allocator's
  global live-range splitting: 22 functions of about 7,500 lines each, every one holding the program's 90 vector
  registers live across a thousand blocks. Nine SPU files take over a minute; one PPU function alone takes two.
- **Copies.** The SPU analyzer starts a function at every call target and copies into it everything it reaches,
  including other functions' code that it branches or falls into: block starts repeat 1.1 to 3.7 times across the 12
  biggest programs (2.4 times in the biggest, 3.7 in 2607cab9).
- **The shared PPU header.** Every unit includes game_ppu.h (85,401 prototypes, 5.7 MB), which takes 0.22 s to parse:
  113 s of the PPU's one-core total.

Found and fixed on the way:
- clang's driver asks for `-disable-free` (the compiler exits right after); in process that leaked each compile's
  AST and module. `DisableFree` is now off in both the front end and code generation options.
- Each compile runs on a thread of its own with clang's 8 MB stack (`clang::DesiredStackSize`): iOS gives other threads
  512 KB.
- After each compile the allocator gives freed pages back (`malloc_zone_pressure_relief`): iOS counts them against the
  app. Repeated compiles of one SPU program stay at 97-500 MB between files (the allocator keeps some; no growth).

## On the iPhone (17 Pro Max, iOS 26, development install; `scripts/codegen-bench.sh`)

The app with the compiler inside (`scripts/build-llvm-ios.sh`, then `scripts/ios.sh build`): LLVM adds 66 MB of code
to the app (__TEXT 149 to 215 MB, with GTA V's built-in translation still in it), and 3.6 MB of headers (clang's arm64
ones, the sysroot, the runtime's ABI headers) in the app's codegen folder.

```
== codegen.compile: C compiled, linked and signed on this device; translated code compiled (route B)
   * small_program_compile_and_link = 50 ms
   compiled, linked and signed here: signature registered, library validation passed; loaded, ps_answer() = 42
   * compile game_ppu_100.c (1.8 MB of C) -O1 = 1.55 s (1.16 MB/s), memory 183 MB, peak +191 MB
   * compile game_ppu_100.c (1.8 MB of C) -O2 = 1.61 s (1.11 MB/s), memory 243 MB, peak +22 MB
   * compile spu_2607cab907577500_04c00_0.c (2.1 MB of C) -O1 = 35.92 s (0.06 MB/s), memory 211 MB, peak +279 MB
   * compile spu_2607cab907577500_04c00_0.c (2.1 MB of C) -O2 = 37.87 s (0.06 MB/s), memory 163 MB, peak +128 MB
   * compile spu_bb7fe62f5ca7873a_04c00_0.c (7.6 MB of C) -O1 = 31.54 s (0.24 MB/s), memory 243 MB, peak +716 MB
   * compile spu_bb7fe62f5ca7873a_04c00_0.c (7.6 MB of C) -O2 = 35.36 s (0.22 MB/s), memory 590 MB, peak +78 MB
   * 2 at once (game_ppu_100.c, -O2) = 2.34 s, 1.53 MB of C per second, peak +151 MB
   * 4 at once (game_ppu_100.c, -O2) = 3.64 s, 1.96 MB of C per second, peak +59 MB
   * 6 at once (game_ppu_100.c, -O2) = 4.57 s, 2.35 MB of C per second, peak +311 MB
   PASS  compile + link + sign + load: ok  [154.47 s]
10 passed, 0 failed, 0 skipped (155.48 s)
end:     thermal nominal, low power mode off, footprint 451 MB
```

- **The whole chain runs on the phone:** C compiled by clang in the app, linked by lld in the app, signed by the app,
  loaded and called (42). Route B needs no Mac step from here on but the translation itself.
- **One performance core of the A19 Pro compiles as fast as the M4's or faster** (PPU 1.6 s against 1.9-2.2 s; SPU
  35-38 s against 35-46 s). Six compiles at once give 2.1 times one (2.35 MB of C a second): the two performance cores
  and four efficiency cores.
- 155 s of full load left the phone at nominal thermal state.
- First runs on the phone failed on two bugs of ours, found only there: lld reserves a signature only for macOS unless
  asked (`-adhoc_codesign` now), and `clang::createInvocation` adds `-fsyntax-only`, which drops `-o` (the object went
  beside the source: a read-only folder on the phone, a lucky match in the Mac's tests). The compile job now comes from
  clang's driver as `clang -c -o` makes it.

GTA V at these settings on the phone: about 823 MB / 2.35 MB/s = 6 min of PPU, plus about 8 min of SPU (the Mac's
1,864 thread-seconds, at the phone's speed and six workers): about 14 minutes, before the fixes below.

## Settings that cut compile time (Mac, one core, LLVM 22 in process unless noted)

| File | Setting | Time | Object |
|---|---|---|---|
| PPU game_ppu_234.c (Apple clang 17) | default | 35.1 s | 183 KB |
| | -fno-slp-vectorize | 3.3 s | 102 KB |
| PPU game_ppu_100.c (Apple clang 17) | default | 1.65 s | 238 KB |
| | -fno-slp-vectorize | 1.37 s | 232 KB |
| SPU 2607cab9 | default | 40.2-40.9 s | 2.41 MB |
| | -fno-slp-vectorize | 42.2 s | |
| | -grow-region-complexity-budget=3000 | 41.0 s | 2.53 MB |
| | -grow-region-complexity-budget=1000 | 17.1 s | 1.70 MB |
| | -grow-region-complexity-budget=300 | 14.1 s | 1.71 MB |
| | -regalloc=basic | 14.4 s | 1.99 MB |

- game_ppu_234.c spends 97% of its optimisation time in the SLP vectorizer, on one function made of a few blocks of
  hundreds of statements each; without it the unit compiles ten times faster and its code is 44% smaller.
- In the SPU programs the greedy allocator's region growing is what costs: a budget of 1,000 (LLVM's default 10,000)
  halves the time and the code shrinks by 30%. Whether the game runs as fast is measured next (stage 3: the Mac builds
  the game library with this same compiler, and the gate compares the SPU and PPU threads' CPU time over the walk).
