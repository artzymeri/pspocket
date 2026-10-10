# pspocket — working rules

A static recompiler that turns PS3 games' PPU and SPU code into native, signed iOS apps: one Mac-side build per game
(disc image in, app out), the runtime shared by all of them (D-036). First and driving title: GTA V (BLES01807,
D-025). The plan is `docs/plan.md`; decisions are `docs/decisions.md`; where things stand and what comes next is
`docs/status.md` (read it first in a new session).

## Rules (plan section 12)

1. **Never commit game data.** No ISOs, ELFs, SELFs, PRXs, firmware, keys or extracted assets, and nothing derived
   from them (inventories, traces, generated code). Test inputs are homebrew built from source or synthetic (built
   in code). Check `.gitignore` and `git status` before any commit that adds files.
2. **macOS first.** Every runtime feature runs and is tested on macOS before any iOS work. iOS-specific code lives
   only in `runtime/platform/ios/` and `app/ios/`.
3. **Oracle before optimizer.** Build the interpreter for an ISA before its lifter. Every lifted instruction gets a
   differential fuzz test against the interpreter.
4. **Scope by inventory.** Implement a syscall or module function only when the inventory or a runtime log shows the
   game uses it. Unimplemented calls log `module::function (NID)` and return an error; they never silently succeed.
5. **Fail loudly in dev.** Unknown indirect targets, unknown SPU hashes, unhandled RSX methods: log the guest address,
   method or hash, then abort in dev builds. Release builds fall back to the interpreter and log.
6. **Game-specific code stays in `games/<title>/`.** No title checks in the runtime. Patches are data (address plus
   bytes, or a function override) with a comment explaining why.
7. **Every fixed bug gets a regression test** when it reproduces without game data.
8. **Measure before optimizing.** Performance changes come with before and after numbers from the same scene.
9. **Small commits per component.** Record every design decision or change in `docs/decisions.md`.
10. **Ask the user for device work** that needs them: unlocking the phone, trusting the developer, Instruments
    traces, playtesting. Say exactly what to run and what to report back. (Building, installing and launching on a
    connected device works from the command line: `scripts/ios.sh`.)
11. **Reference, don't blindly copy.** The licence is GPLv2 (D-001), so RPCS3 code may be reused, but cite the
    reference file in a comment whenever semantics come from it. Where hardware manuals and RPCS3 disagree with real
    games, real games win.

## Setup (once per clone)

```sh
git config core.hooksPath .githooks   # pre-commit refuses PS3 executables, PKGs, SFOs, disc images, key-like text
```

Outside sources the build reads live in `external/` (gitignored, never committed, D-201): `external/rpcs3` (RPCS3 with
its libpng submodule: the SELF keys, libpng) and `external/llvm-project` (the LLVM revision build/llvm-host was built
from). Anything else from outside goes there too, never beside the repository.

## Build and test

```sh
cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON
ninja -C build/host
ctest --test-dir build/host --output-on-failure
```

- Clang only (D-026). On Linux, prefix the configure step with `CC=clang CXX=clang++`.
- FFmpeg's decoders for the media libraries: `scripts/build-ffmpeg.sh [macos|ios|linux]` (the release tarball in
  `build/ffmpeg/src/`) into `build/ffmpeg/<target>`, found by the configure step (D-181, D-576).
- AArch64-only paths (lifted VMX, SPU FP) on an x86 Linux host: cross-build and test under qemu with
  `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64-qemu.cmake` into `build/arm64` (D-027).

- Host tools land in `build/host/`: `tools/ps3inspect/ps3inspect`, `recomp/ppu/ppu-lift`,
  `runtime/platform/macos/pspocket-selftest`.
- Phase 0 inventory: `ps3inspect --json inventory/gta4/eboot.json EBOOT.elf` (`inventory/` is gitignored; see
  `docs/phase0.md`). NID/syscall names: `recomp/common/scripts/harvest_nids.py <rpcs3-dir> [--check]` (D-019).
- Focused builds while other work is in progress: `-DPSPOCKET_ONLY="runtime/core;recomp/common;tests/common"`.
- One command per game: `scripts/translate-game.sh --iso GAME.iso [--ios]` decrypts the disc's EBOOT.BIN (D-121),
  translates its PPU and SPU code into inventory/<TITLE_ID>/ and builds it in (D-101).
- PPU lifter: `ppu-lift --elf EBOOT.elf --out DIR` (a whole executable, D-101) or
  `ppu-lift --image f.bin --base 0x10000 --seed 0x10200 --out DIR [--tu N] [--strict]` (D-023).
  `ppu-dis --image f.bin --base 0x10000 ADDR [COUNT]` disassembles; a fault in translated code prints host frames for
  `atos -o pspocket-boot -s SLIDE ADDR...` (D-094).
  Longer differential fuzzing: `PSPOCKET_FUZZ_SEED=<n> PSPOCKET_FUZZ_ITERS=<k> build/host/tests/ppu/test_ppu_fuzz`.
  Never build lifted code with `-ffast-math`/`-Ofast`.
- Disc images: `ps3disc <image.iso> <out-dir>` (host) or the app's `-extract-iso` mode pulls out the executables only;
  `ps3disc --decrypt <image.iso> <out.elf>` decrypts EBOOT.BIN, as the app does on the phone (D-121). The keys come
  from RPCS3's source at build time (`scripts/self-keys.py`, `-DPSPOCKET_RPCS3_DIR`) into the build directory only.
- One bring-up round on the Mac: `scripts/boot.sh` (pull, build, run the decrypted EBOOT with the disc image read in
  place, print the gaps). It finds the game's .iso (or copies it once from the iPhone app: `scripts/ios.sh pull-iso`).
- Boot an executable under the interpreter (Phase 2 bring-up, D-033): `pspocket-boot EBOOT.elf [--bdvd DIR|IMAGE.iso]
  [--hdd0 DIR] [--timeout S] [--stop-at-first] [--imports]`; it prints every unimplemented syscall and firmware
  function it hit (`--imports` lists the imports and their state without running). Runs go in `docs/runs/`.
- iOS test host: `scripts/ios.sh build|install|run` (XcodeGen project in `app/ios/project.yml`, team M7XJA39ZV6).
  An iPhone that is neither connected nor on this Mac's network: `scripts/ios.sh testflight` archives and uploads to
  App Store Connect for TestFlight (internal testers). It sends the app, with the game's translated code, to Apple:
  only with the user's OK, each time.
- Use your own build directory (`build/<name>`) when several builds run at once.

## Layout

```
tools/ps3inspect/        Phase 0 inventory tool
recomp/common/           ELF/SELF/PRX loader, NID database, manifest format, hashes
recomp/ppu/              PPU analyzer, lifter, C code generation
recomp/spu/              SPU extractor, analyzer, lifter (Phase 3)
runtime/core/            guest memory, big-endian accessors, logging, timebase
runtime/ppu/             PPU context, decoder, interpreter (oracle + fallback), dispatch table
runtime/lv2/ spu/ rsx/   lv2 kernel, SPU runtime, RSX front end and Metal backend
runtime/hle/             firmware modules on the host (HLE), import linkage, main-area allocator, TLS
runtime/loader/          ELF executable -> lv2 process; pspocket-boot
runtime/selftest/        device self-test probes with a C API (used by the iOS app and macOS CLI)
runtime/platform/macos/  macOS dev host (pspocket-selftest CLI)
runtime/platform/ios/    iOS host glue
app/ios/                 SwiftUI test host / app shell (XcodeGen)
games/gta4/              manifest: hashes, overrides, patches, SPU table, shader list
tests/                   Catch2 suites per component
docs/                    plan.md, decisions.md, phase notes
```

## Conventions

- C++20 for toolchain and runtime (namespace `pspocket`, sub-namespaces per component). Generated code is C11.
  C APIs use the `ps_` prefix; macros use `PS_`.
- Guest memory access is always `base + (uint32_t)addr` with an explicit byte swap: use `pspocket/core/be.h`.
- 128-bit vectors (VMX, SPU) use the full-reverse lane order (D-004).
- Logging: `PS_LOG_INFO("chan", "fmt {}", x)` (std::format). Dev aborts: `PS_FATAL`. Invariants: `PS_CHECK`.
- Tests: Catch2 v3 (`#include <catch_amalgamated.hpp>`), registered with `pspocket_add_test()` in `tests/<component>/`.
- Warnings are errors in CI builds (`-DPSPOCKET_WERROR=ON`); keep the tree warning-free.
