# pspocket — working rules

A per-game static recompiler that turns a PS3 game's PPU and SPU code into a native, signed iOS app.
First title: GTA IV (BLES00229, update 1.08). The plan is `docs/plan.md`; decisions are `docs/decisions.md`.

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

## Build and test

```sh
cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON
ninja -C build/host
ctest --test-dir build/host --output-on-failure
```

- Host tools land in `build/host/`: `tools/ps3inspect/ps3inspect`, `recomp/ppu/ppu-lift`,
  `runtime/platform/macos/pspocket-selftest`.
- Phase 0 inventory: `ps3inspect --json inventory/gta4/eboot.json EBOOT.elf` (`inventory/` is gitignored; see
  `docs/phase0.md`). NID/syscall names: `recomp/common/scripts/harvest_nids.py <rpcs3-dir> [--check]` (D-019).
- Focused builds while other work is in progress: `-DPSPOCKET_ONLY="runtime/core;recomp/common;tests/common"`.
- iOS test host: `scripts/ios.sh build|install|run` (XcodeGen project in `app/ios/project.yml`, team M7XJA39ZV6).
- Use your own build directory (`build/<name>`) when several builds run at once.

## Layout

```
tools/ps3inspect/        Phase 0 inventory tool
recomp/common/           ELF/SELF/PRX loader, NID database, manifest format, hashes
recomp/ppu/              PPU analyzer, lifter, C code generation
recomp/spu/              SPU extractor, analyzer, lifter (Phase 3)
runtime/core/            guest memory, big-endian accessors, logging, timebase
runtime/ppu/             PPU context, decoder, interpreter (oracle + fallback), dispatch table
runtime/lv2/ hle/ spu/ rsx/   later phases
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
