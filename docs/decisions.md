# Decisions

Every design decision or change, newest last. Format: ID, date, decision, why, consequences.
Open decisions from plan section 14 are tracked at the bottom.

## D-001 · 2026-10-03 · Licence: GPLv2

The plan's recommendation. The repo is private, so GPLv2 costs nothing, and it allows reusing RPCS3 code
(lv2 semantics, the RSX shader decompilers, the guest memory map) instead of clean-rooming it.
Consequences: `LICENSE` is GPLv2. Code taken or adapted from RPCS3 keeps a comment naming the source file.
Revisit only if the project ever needs a permissive licence (it would then need a clean-room rewrite of those parts).

## D-002 · 2026-10-03 · Game version: GTA IV BLES00229 (EU) with update 1.08 (tentative)

Tentative target, chosen because the `ps3-ios-engine` project worked with BLES00229 and the 1.08 update package
(`EP1017-BLES00229_00-GTAIVUPDATEV0108-A0108-V0100-PE.pkg`) is on the Mac. **The user has no GTA IV dump at the
moment**, so Phase 0's inventory and every GTA IV-specific step wait until there is a dump of a copy the user owns;
confirm region and version against that disc then. The recompiled EBOOT would be the one from update 1.08, which
replaces the disc's. Until then the toolchain and runtime are exercised with synthetic programs and homebrew, which
is what Phases 1–4 do first anyway. The manifest (`games/gta4/manifest.toml`) records hashes once the files exist.

## D-003 · 2026-10-03 · Host and languages

Toolchain and runtime: C++20, built with CMake + Ninja on macOS (Apple Silicon); Linux supported for the toolchain.
Generated code: C11 with `<arm_neon.h>` where needed, compiled by Apple clang. App shell: Swift/SwiftUI.

## D-004 · 2026-10-03 · 128-bit lane order: full reverse

VMX (PPU) and SPU registers are held in host order as a full 16-byte reverse of the guest's big-endian bytes:
host byte `j` = guest byte `15 - j`. So guest element `i` of an `N`-element vector is host lane `N - 1 - i`, already
in host byte order, and element-wise arithmetic maps straight to NEON. Loads and stores (`lvx`, `stvx`, `lqd`, `stqd`)
are a 16-byte aligned access plus one byte reverse (`ps_ld128_rev` / `ps_st128_rev` in `runtime/core/include/pspocket/core/be.h`).
Byte-indexed operations (`vperm`, `lvsl`, `lvsr`, `vsldoi`, SPU `shufb`, the SPU preferred slot) must translate guest
byte index `b` to host byte `15 - b`. Same convention as XenonRecomp and RPCS3's `v128` (confirmed by the Phase 0
evaluation, `docs/phase0-recomp-evaluation.md` §3).

Element-position operations need the same translation: merges, packs and unpacks; even/odd multiplies (a guest even
element is a host odd lane); sums across lanes and splat-by-index; `lvewx`/`stvewx`, `lvlx`/`lvrx`/`stvlx`/`stvrx`;
`mfvscr`/`mtvscr` (VSCR is guest word 3 = host lane 0); SPU `gb*`, `fsm*`, the `cbd` family and every preferred-slot
scalar (host lane 3 for words). Whole-register byte shifts map to native shifts (the reversed register is the guest's
128-bit value as a little-endian integer). `vperm`/`shufb` should become `vqtbl2q_u8({vB, vA}, vbicq_u8(0x1F, vC))`
(RPCS3's PANDN trick), to be verified by the fuzz tests.

## D-005 · 2026-10-03 · std::format instead of fmt

Apple clang 17's libc++ ships `std::format` (iOS 17 / macOS 14 deployment targets are above its availability floor),
which removes one dependency from the plan's list.

## D-006 · 2026-10-03 · Tests: Catch2 v3.16.0, vendored

The amalgamated build (`third_party/catch2/`, BSL-1.0) needs no network at configure time and no package manager.
Each component has a test executable registered with CTest through `pspocket_add_test()` (`tests/CMakeLists.txt`).

## D-007 · 2026-10-03 · iOS builds the runtime through XcodeGen

`app/ios/project.yml` defines a static-library target compiled from the same `runtime/` sources the CMake host build
uses, and the app links it. One `xcodebuild` builds and signs everything; no separate iOS CMake toolchain to keep in
sync. The host CMake build stays the source of truth for tests.

## D-008 · 2026-10-03 · Minimal iOS test host pulled forward to Phase 0

The plan brings a minimal iOS test host in at Phase 4. A smaller one (self-test probes, a Metal view with frame
times, no game) lands now, because the riskiest platform assumptions can only be checked on the device: signing with
the memory entitlements, reserving 4 GB of guest address space, fault-based write tracking, and the real speed of
interpreted versus lifted PPU code on the A19 Pro. Each later phase adds its probes.

## D-009 · 2026-10-03 · Lifted function signature (amended by D-013)

`void f_XXXXXXXX(PPUContext* restrict ctx, uint8_t* base)` where `XXXXXXXX` is the guest entry address in hex. The
guest base is an argument (as in XenonRecomp) so it stays in a register through call chains instead of being reloaded
from the context. Context registers are loaded into locals at entry and spilled around calls and syscalls.
XenonRecomp-style ABI-trusting register modes may be added later as manifest-controlled options, once the oracle passes.

## D-010 · 2026-10-03 · Host page granularity

Apple silicon uses 16 KB pages (macOS and iOS). Guest memory is committed and protected at 16 KB granularity; guest
4 KB page semantics that need finer control (stack guard pages, 4 KB write tracking) are handled in software where a
game actually depends on them.

## D-011 · 2026-10-03 · Primary test device and signing

iPhone 17 Pro Max (A19 Pro, iOS 26), team M7XJA39ZV6 (paid, already proven with the two memory entitlements in
`ps3-ios-engine`). Bundle IDs live under `com.artzymeri.pspocket`.

## D-012 · 2026-10-03 · Toolchain: our own lifter; ps3recomp is not reused

From the Phase 0 evaluation (`docs/phase0-recomp-evaluation.md`). ps3recomp (MIT) fails silently where our rules
demand loud failures (unknown instructions become no-ops, unresolved imports return CELL_OK), has no interpreter
oracle, uses a vector layout that contradicts D-004 with open lane bugs, runs no game on macOS and has no iOS support.
It also ships decrypted Sony firmware and keys, so nothing is copied from it; at most its pitfall list and synthetic
jump-table cases are used as test ideas, re-checked against RPCS3. XenonRecomp (MIT) is the design reference for the
emitter, VMX handling, config/hook/override mechanisms, but it encodes Xenon assumptions (32-bit compares, 32-byte
`dcbz`, no-op fences, unscaled `mftb`) that do not hold on the Cell. RPCS3 stays the authority for semantics.

## D-013 · 2026-10-03 · Guest accessors are volatile; `base` is not restrict-qualified

Guest code polls memory that other host threads write (SPU completion flags, cellSync, RSX labels, the FIFO get
pointer). `ps_readN`/`ps_writeN` in `be.h` are therefore volatile accesses through byte-aligned types: the compiler
cannot hoist, merge or drop them, unaligned guest addresses stay well-defined, and on ARM64 each is still one
`LDR`/`STR` (+ `REV`) with `base + uxtw(addr)` folded into the addressing mode. `restrict` is dropped from `base` in
the lifted signature, because HLE code and other threads legitimately touch guest memory during a lifted call. Lifted
prologues assume `base` is 64 KB-aligned. Host-pointer accessors (`ps_ldN`/`ps_stN`) stay plain. Cost on the M4:
scalar big-endian loads 12 GB/s (the earlier 50–60 GB/s came from auto-vectorization lifted code can't use anyway).

## Open decisions (plan section 14)

- [x] Licence → D-001.
- [~] Game version → D-002 (tentative: no dump yet).
- [x] Toolchain → D-012 (write our own).
- [ ] SPURS: HLE or LLE. Decided by the Phase 0 inventory.
- [~] Target devices: primary test device is D-011; minimum chip and the iPad question are still open.
- [ ] Input: controller required, or a polished touch overlay as well.
- [ ] ISO handling: extract assets, or read directly from the ISO.
