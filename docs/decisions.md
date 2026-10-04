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

## D-014 · 2026-10-03 · iOS build and run flow

`scripts/ios.sh` is the entry point (`gen`, `build [--sim]`, `install`, `run`, `report`, `all`). It builds the host
CMake tree first, so build-time generated sources exist, then runs XcodeGen and `xcodebuild`. A runtime component joins
the iOS build when its `CMakeLists.txt` exists (as `pspocket_add_component` does on the host); every `.c`/`.cpp` in its
`src/` and in `<host build>/generated/selftest/` is compiled, so host-only code never goes in a runtime `src/`. The
runtime library is force-loaded into the app so self-registering code isn't dropped. Release everywhere, including the
Xcode scheme, because the probes' numbers only mean something optimised. Device signing uses automatic provisioning
(`-allowProvisioningUpdates`), which needs Xcode signed in to team M7XJA39ZV6.

## D-015 · 2026-10-03 · Self-test threading on iOS

Probes run on a dedicated thread with an 8 MB stack at userInitiated QoS, matching the macOS CLI's main thread. GCD
threads have 512 KB, and lifted code nests host calls as deep as the guest nests guest calls. The same applies later to
PPU threads: their host stacks must be sized for guest call depth.

## D-016 · 2026-10-03 · Output preview and frame pacing

The test host renders a 1152×640 bgra8 target, upscales it with MetalFX spatial (perceptual) to an aspect-fit texture,
and composites it letterboxed; a CADisplayLink at 30/30/30 drives frames, presented with
`present(atTime: link.targetTimestamp)`, and frame intervals come from `presentedTime`. MetalFX falls back to a
bilinear blit where unavailable (the simulator), labelled on screen. This is the shape the RSX backend's output stage
will take (plan section 7).

## D-017 · 2026-10-03 · iOS logging

`pspocket::log` goes to os_log with subsystem `com.artzymeri.pspocket` and the log channel as category; Info/Warn map to
default, Error to error, Fatal to fault. The test host also mirrors lines to stdout (visible through
`devicectl ... --console`) and into the self-test report.

## D-018 · 2026-10-03 · Build the game-independent engine before the Phase 0 inventory

The user asked for the whole engine to be built now rather than waiting for a GTA IV dump. Work that every PS3 title
needs proceeds ahead of the Phase 0 gate: PPU VMX, the SPU interpreter and lifter, the shared reservation system,
the core lv2 kernel (threads, synchronization, memory, time, filesystem, events), and the RSX command parser and Metal
backend. Rule 4 still holds for everything title-specific: firmware modules beyond what homebrew and every game use,
SPU program replacements, patches, and the GTA IV bring-up wait for the inventory. Phase gates remain the measure of
done; they are no longer the order in which work starts.

## D-019 · 2026-10-03 · NID and syscall names come from RPCS3, by script

`recomp/common/scripts/harvest_nids.py <rpcs3-dir>` regenerates `recomp/common/data/nids.txt` (6,625 names in 148
modules) and `data/syscalls.txt` (634 syscalls) from RPCS3's registration macros and syscall table. They are API
names, so they are committed and embedded into `pspocket::recomp_common` at build time; more names files can be loaded
at runtime (`ps3inspect --names`). Rerun after RPCS3 updates; `--check` detects drift. The runtime gets its own
committed syscall-name table generated from the same file (it can't depend on recomp/).

## D-020 · 2026-10-03 · Toolchain parsers never abort on bad input

ELF, SELF, PRX, TOML and manifest parsing use bounds-checked reads and return errors as values; `PS_CHECK` is only for
programming mistakes. Robustness tests (truncations, 60k random mutations) run under ASan and UBSan. SELF handling reads
the plaintext headers only and reports the licence type; it never decrypts. A PRX is viewed statically with each
segment at its `p_vaddr` and relocations applied the way RPCS3's `ppu_load_prx` does.

## D-021 · 2026-10-03 · Manifest format, SPU program identity, Phase 0 outputs

- `games/<title>/manifest.toml` is a strict TOML subset: unknown keys are errors, every patch needs a `why`,
  overlapping patches are rejected, hashes are optional in drafts and required for builds, paths are relative to a
  dump laid out like the PS3 filesystem (`dev_bdvd/`, `dev_hdd0/`).
- SPU programs are keyed by the SHA-1 of their ELF image bytes. RPCS3's `SPU-`/`PPU-` hashes are printed alongside so
  results can be matched against RPCS3 logs. The key for code loaded at runtime (overlays, SPURS jobs) is decided in
  Phase 3.
- Phase 0 outputs (`ps3inspect --json`, traces) live in `inventory/<title>/`, which is gitignored; only the decisions
  and the re-estimate go into docs. `ps3inspect` exits 0 (ok), 1 (usage or I/O error), 2 (bad input), 3 (encrypted
  SELF).

## D-022 · 2026-10-03 · Phase 0 runs on GTA V (BLES01807), the dump on hand

The user's dump is GTA V, not GTA IV: BLES01807 (EU), disc version 01.03, app 01.00, 18.74 GB, decrypted at disc
level (its region table still marks 18.47 GB encrypted; the EBOOT reads as a SELF, so the content isn't). It lives on
the phone in the app's Documents; `-extract-iso` copied out PS3_DISC.SFB, PARAM.SFO and EBOOT.BIN (14.6 MB, a disc
SELF with no NPDRM header: no RAP needed). GTA V ships no separate PRX modules. The Phase 0 inventory runs on this
EBOOT once RPCS3 has decrypted it; whether the first title becomes GTA V instead of GTA IV (D-002) is decided from
that inventory. GTA V is the heavier game (more SPU work, larger engine), so the go/no-go matters more, not less.

## D-023 · 2026-10-03 · PPU interpreter and lifter conventions

- **Registers in lifted code:** CR is `uint8_t cr[32]` (one byte per bit) and XER is separate so/ov/ca/bc bytes, both
  in the context and in locals; FPRs are doubles. A lifted function loads every register it references at entry,
  spills the ones it wrote before calls and returns, and reloads after calls; nothing relies on the ABI.
- **FPSCR:** the architected 32-bit register. Writes follow the architecture (FEX/VX recomputed, `mtfsb1` on an
  exception bit sets FX, `mcrfs` clears what it copies); `fcmpu`/`fcmpo` set FPCC; arithmetic sets no FPRF or
  exception bits and rounds to nearest (as RPCS3 by default); only `fctiw`/`fctid` honour RN. Rc=1 FP forms set
  CR1 = FX FEX VX OX as the architecture says (RPCS3 differs). The interpreter warns once if a guest sets RN ≠ 0.
- **Deviations from RPCS3, on purpose:** D-form with rA = 0 uses a literal 0; `fctiw` honours RN; the FPSCR
  instructions are complete. `stfs` rounds like RPCS3 (hardware truncates).
- **Dispatch:** one 8-byte slot per 4-byte code address over the registered range; empty slots point at a miss stub,
  so an indirect call is a bounds check plus one load. Miss policy: abort without NDEBUG, interpret with NDEBUG
  (configurable). One range of up to 256 MB today; PRX modules will need more ranges.
- **Interop contract:** a lifted function behaves as "run from the entry until `blr`", then execution continues at
  LR. An interpreter fallback stops when cia equals the entry LR and r1 equals the entry SP.
- **Jump tables:** a `switch` on the computed CTR value whose `default` traps (`ps_ppu_jump_table_miss`).
- **Fuzzing:** whole-context comparison, bitwise, except that FP-arithmetic forms accept any NaN for any NaN.
- **Names:** lifted functions are `<prefix>f_XXXXXXXX` (upper-case hex) with a per-image `<name>_register()`.
- **Build flags:** lifted code is never compiled with `-ffast-math`/`-Ofast`; `lifted.h` disables FP contraction.
- **Open:** VMX is decoded but not executed; reservations still use a single-thread model in the context
  (`ps_ppu_lwarx`/`stwcx` in runtime/ppu/src/runtime.cpp) until they move onto `reservation.h`.

## D-024 · 2026-10-03 · Phase 0 result for GTA V: go, SPURS by LLE (proposed)

From `docs/phase0-gta5.md`: 85,342 functions / 6.09 M PPU instructions, every scalar instruction already supported,
448 k VMX instructions over 98 mnemonics (in progress), 362 firmware imports all named (184 are online/PSN/trophy
non-goals, stubbed as "offline"), 8 embedded SPURS tasks (494 KB), and job chains that only a runtime trace will
reveal. **Go**, with SPU throughput on the phone as the decisive risk and a checkpoint at the first lifted physics task.
**SPURS: LLE proposed** — recompile Sony's libsre (PPU + SPU) from the user's own firmware file like game code,
because GTA V uses tasksets and job chains (RPCS3 relies on LLE for these); HLE stays the fallback. Pending the
user's firmware file. Raw inventory outputs stay in `inventory/`; the aggregate report goes to docs (amends D-021).
The analyzer recognised no jump tables in this EBOOT (SNC rather than GCC patterns, to investigate).

## D-025 · 2026-10-03 · First title: GTA V (BLES01807), confirmed by the user

The user owns a GTA V dump and no GTA IV dump, the inventory says go, and the user confirmed the switch.
`games/gta5/manifest.toml` is the active manifest; `games/gta4/` stays as a draft. Firmware for SPURS LLE (D-024):
the user approved using Sony's official PS3UPDAT.PUP; it is placed by the user (this session's tooling may not
fetch it) under `inventory/firmware/`, outside git.

## D-026 · 2026-10-03 · Clang only; the tree builds and tests on Linux

Cloud sessions run on Linux, so the host build (toolchain, runtime, tests) must work there too, not only on the Mac.
The tree now builds and passes ctest on Linux with LLVM Clang: `sys/sysctl.h` is included on Apple only, the QoS
probe's helper is unused elsewhere, and 128-bit integers go through `s128`/`u128` in `pspocket/core/types.hpp`
(`__extension__`, so `-Wpedantic` stays quiet). GCC is rejected at configure time: the interpreter relies on
`#pragma clang fp contract(off)` for bit-exact FP (GCC would silently fuse multiply-adds), and GCC 13's
`-Wnull-dereference` fires inside libstdc++. Apple Clang remains the reference compiler.

## D-027 · 2026-10-03 · AArch64 tests run under qemu on x86 Linux; host-independent stack layout

The lifted VMX helpers (`pspocket/ppu/vmx.h`) are NEON and the SPU FP environment is AArch64 FPCR (D-003), so an
x86 host can't run those paths natively. `cmake/toolchains/linux-aarch64-qemu.cmake` cross-builds the tree with
Clang for aarch64-linux-gnu and runs the build-time generators and ctest through qemu-user; the whole suite passes
there, VMX fuzzing included. x86 builds stay supported for everything else: the PPU fuzz test lowers its form-count
floor when the cases were generated with `--no-vmx`, and the SPU FP environment adds MXCSR FTZ/DAZ to the rounding
mode on x86-64 (default-NaN behaviour remains the host's; best effort).

lv2 thread stacks are laid out in 16 KB units on every host (size rounded up and a 16 KB guard below), not in host
pages: guest-visible stack addresses must not differ between a 4 KB-page host and the phone.

## D-028 · 2026-10-03 · PPU VMX: RPCS3 accuracy model, NEON lifted code, scalar oracle

Recorded after the fact for the agent work merged in 8e99d87; full detail in `pspocket/ppu/vmx.h`.
All VMX instructions execute in the interpreter (`runtime/ppu/src/interpreter_vmx.cpp`, scalar, per element) and
lift to NEON (`vmx.h`, one static inline function per instruction, `uint32x4_t` locals in D-004 order); the
differential fuzz compares them bit for bit, NaN payloads included. Semantics are RPCS3's with its accuracy options
on: VSCR[NJ] flushes denormals (inputs and the results RPCS3 flushes), NaN results are the first NaN operand
(vA, vB order, quieted) else 0x7fc00000, VSCR[SAT] is sticky (a vector accumulator in lifted code). vrefp,
vrsqrtefp, vexptefp and vlogefp are correctly rounded (host libm), not the hardware estimates. mfvscr/mtvscr use
guest word 3 as the architecture does (RPCS3's interpreter uses word 0). Lifted VMX is AArch64-only (D-003); x86
hosts fuzz it under qemu (D-027).

## D-029 · 2026-10-03 · SPU: interpreter, lifter, channels and MFC; FP model in the host FP environment

Detail in `pspocket/spu/fp.h`, `context.h`, `runtime.h` and `recomp/spu/.../lifter.hpp`. SPU code runs with the
host FPCR set to FZ + round-toward-zero + DN, entered once per SPU thread, so single-precision arithmetic is native.
Two single-precision models, per context: APPROXIMATE (default; RPCS3's default xfloat: IEEE single in that
environment, with RPCS3's fm/fma/fs fixups) and ACCURATE (extended range in double precision). Double precision
rounds to nearest inside one FPCR window per instruction. frest/frsqest/fi are RPCS3's tables, bit-exact. Lifted
programs are keyed by a hash of their LS code range, entered at any block start, and fall back to the interpreter
(logging the address) for unknown targets. Channels, mailboxes, signals, events, the decrementer and MFC DMA/list
commands live in the runtime; contexts without a host thread stop with PS_SPU_STOP_BLOCKED instead of blocking.

## D-030 · 2026-10-03 · One reservation system for PPU and SPU: seqlocked 128-byte line versions

Detail in `runtime/core/src/reservation.cpp` (contract: `pspocket/core/reservation.h`). 32768 version slots, one per
128-byte host cache line, hashed from the guest line index. A slot is a seqlock and a stripe lock at once;
conditional stores compare the whole line against the snapshot and write changed 8-byte words with CAS, so plain
stores (which take no lock) are never lost. Waits with timeouts use parked mutex/condvar buckets counted per slot,
so an uncontended store costs one extra load.

## D-031 · 2026-10-03 · lv2: host threads, no scheduler emulation, VFS over host directories

Detail in `pspocket/lv2/kernel.hpp` and the source headers in `runtime/lv2/src/`. Every PPU thread is a host thread
(8 MB stack, D-015) running lifted code with interpreter fallback; blocking syscalls block it; priorities only order
SYS_SYNC_PRIORITY wait queues and choose host QoS. Thread exit unwinds through lifted code with a C++ exception, so
lifted code must keep unwind tables. Memory follows RPCS3's area layout (256 MB user areas, stacks in
0xD0000000..0xE0000000, D-027 for their granularity). sys_fs maps mount points to host directories with normalised
paths (no escape from a mount), no NPDRM, case-sensitive like iOS. Not yet: interrupt-thread establishment,
sys_mmapper access-right changes and page-fault notification.

## D-032 · 2026-10-03 · RSX: portable front end, Metal backend, write tracking by protection or hash

Detail in `pspocket/rsx/rsx.hpp`, `shader/ir.hpp`, `write_tracker.hpp`. The FIFO parser, method table, register
state, shader microcode decoding (VP/FP to an IR, then MSL) and texture layouts are portable C++ tested on any host;
the Metal backend (`runtime/rsx/metal/`, Objective-C++ with ARC) builds on Apple platforms only. Semaphores, labels
and reports are written in command order. Written guest memory behind GPU copies is detected either by page
protection (the plan's design; 3.7 µs per fault on the A19 Pro) or, until the platform fault handler is installed,
by content hashes.

## D-033 · 2026-10-03 · HLE linkage and the process loader; pspocket-boot for Phase 2 bring-up

`runtime/hle` holds the firmware modules implemented on the host and the per-process linkage; `runtime/loader` turns
a decrypted ELF into a running lv2 process; `pspocket-boot` runs it under the interpreter on any host.
- **Imports** link the way RPCS3 links HLE functions (PPUModule.cpp): one 8-byte entry per function in a main-area
  block, `{func = entry + 4, toc = 0}`, the zero TOC word doubling as the stub instruction. The stub address is
  registered in the PPU dispatch table with a trampoline that identifies the import from `ctx->cia`, so lifted code
  and the interpreter reach HLE through the ordinary indirect-call path, with no special instruction. Executing the
  stub word directly is an illegal instruction (fails loudly). Unimplemented functions log
  `UNIMPLEMENTED module::function (NID)` with the caller and r3..r6 and return CELL_ENOSYS, or abort with
  `--stop-at-first`; imported variables are logged and left unpatched. Reference tables (attribute 0x2000) are
  patched with RPCS3's ppu_patch_refs types 1/4/6/57.
- **Registration** is explicit (`register_<module>()` listed in `runtime/hle/src/modules.cpp`), not static
  initialisers, which a static-library link drops. NIDs are written in the source and checked against `nid_of(name)`
  by the tests, so the runtime does not need the toolchain's SHA-1 and NID database.
- **Process start** follows RPCS3 ppu_load_exec with liblv2 in HLE mode (no firmware needed): segments at p_vaddr in
  the main area (256 MB, or 512 MB for SDK < 2.0), main thread with sys_process_param's priority and decoded stack
  size, detached; `sysPrxForUser::sys_initialize_tls` runs on the main thread before the entry (TLS blocks of
  0x30 + p_memsz in a 256 KB main-area region, r13 = block + 0x7030); argv/envp as u64 pointer arrays at the stack
  top; entry registers r3..r12 as RPCS3 sets them.
- **Allocation in the main area** (`hle::MainMemory`, RPCS3 vm::alloc in vm::main) is first fit at 64 KB alignment
  in whole host pages, after the loader reserves the executable's segments.
- Not yet: PRX loading (the game's own modules, LLE firmware libraries), write protection of read-only segments,
  charging the segments to the default memory container. The loader uses recomp/common's ELF and module parsers;
  the iOS app will need them on device too (the EBOOT comes from the user's disc), to be wired in when the app boots
  a game.

## D-034 · 2026-10-03 · Bring-up loop: boot log → HLE functions; guest faults reported, not fatal-silent

GTA V's first boot (docs/runs/004) set the loop for Phase 2: run `pspocket-boot`, implement what the log names, run
again. Decisions from the first round:
- **sysPrxForUser lwmutex** is RPCS3's user-space protocol (sys_lwmutex_.cpp without its `hle_lwmutex` option):
  atomics on the guest's big-endian owner/waiter word, the kernel sleep queue (`_sys_lwmutex_*`, already in lv2) only
  on contention, the `reserved` owner value handing the lock to the woken waiter. The whole set (create, destroy, lock,
  trylock, unlock) lands together because a created lwmutex is only used through it. HLE functions get guest
  scratch variables on the caller's stack (RPCS3 vm::var: r1 moves below them for their lifetime).
- **null_func syscalls**: lv2 binds a syscall that RPCS3 answers with `null_func` (CELL_OK, logged) only when a game
  is seen calling it, with RPCS3's table comment as its description (`SyscallTable::bind_null`). First: 988.
- **Guest faults** in `pspocket-boot` print the guest address, the PPU thread and its cia/lr/r1/r2/r3 and exit with
  128 + signal (a debugger still stops at the fault first). The runtime-wide handler (write tracking, release
  fallback) is the RSX write tracker's job later; this one is for bring-up.
- Unimplemented HLE calls are logged like unimplemented syscalls: the first 8 calls, then every 1024th.

## D-035 · 2026-10-03 · cellSysmodule is bookkeeping; HLE modules keep per-process state

Run 005. Every imported firmware function is linked to HLE (or the UNIMPLEMENTED stub) at load time, so
`cellSysmoduleLoadModule` does not load anything: it keeps RPCS3's per-module load counts, loads dependencies first
and unloads them last, refuses 0x58, and counts lv2's default modules (SYSUTIL, GCM_SYS, AUDIO, IO, SPURS, and FS from
SDK 3.3) as loaded from the start, exactly as RPCS3's cellSysmodule.cpp does minus its sys_prx calls. The module table
is generated from RPCS3's MODULE_INFOS. Internal IDs (0xF000+) return CELL_SYSMODULE_ERROR_UNKNOWN with an error log
until a game uses one. Module state lives in `HleProcess::state<T>()` (RPCS3's g_fxo), so tests and future
restarts get a fresh copy per process.

## D-036 · 2026-10-03 · Goal: any PS3 game, one Mac-side build per game; GTA V stays the driving title

The user widened the goal from one title to any PS3 disc image. What that means under iOS's rules:
- **Still static recompilation, one build per game.** iOS gives apps no JIT, so a game's PPU and SPU code is lifted
  and compiled on the Mac (disc image in, signed app out). The interpreter remains the fallback for code the build
  missed, ~12× slower (run 003), too slow to be the main path. A phone-only "drop an ISO in" flow isn't possible.
- **The runtime is shared by every game**: lv2, HLE modules, SPU, RSX→Metal. Rule 6 already keeps it title-free; HLE
  and syscall coverage grows with each game's boot log, as RPCS3's did (rule 4 applies per title).
- **GTA V stays the first and driving title**; its gaps are worked first. Other titles follow once it boots.
- Consequence for SPURS (amends D-024): a native SPURS has to cover every SPURS feature any game uses, and RPCS3 has
  no reference for two of them (job chains: `spursJobChainEntry` is a stub; SPURS queues: UNIMPLEMENTED). See the
  open question below.

## D-037 · 2026-10-03 · SPURS is native: our own C++ implementation, no Sony code in the app

The user chose a native SPURS (supersedes the LLE proposal of D-024 and the baseline idea in D-036). cellSpurs is an
HLE module like the others: the PPU API on the host, and a host scheduler that runs each workload's SPU code (task
ELFs, job binaries; lifted like any SPU program) on SPU contexts, without Sony's SPU-side SPURS kernel. Guest-visible
structures (CellSpurs, attributes, tasksets, event flags, ...) keep RPCS3's layouts (cellSpurs.h), because games and
their SPU code read them. Reference: RPCS3 cellSpurs.cpp / cellSpursSpu.cpp where they implement a feature. Where they
don't (job chains: `spursJobChainEntry` is empty; SPURS queues: UNIMPLEMENTED), the reference is Sony's libsre from the
user's firmware file, read on the Mac to learn the behaviour and used as a test oracle, never shipped or committed.
RPCS3's SPU side is a reference for structures only: its native task and kernel entry points are commented out and it
runs libsre's policy-module code even in HLE mode. Architecture and stages: docs/spurs-native.md. Stage 1 (attributes,
initialize, finalize) does not create the SPU thread group, handler threads, lv2 semaphores or the guest lwmutex/lwcond
that libsre's kernel uses; those CellSpurs fields stay 0.

## D-038 · 2026-10-03 · sys_vm without paging (as RPCS3)

Run 006. `sys_vm_memory_map` reserves a 256 MB-aligned area, commits the whole virtual size at once and charges only
the physical size to the memory container; nothing is paged, so lock/unlock/touch/flush/invalidate/store/sync only
validate their range, `sys_vm_test` reports every page on memory and the statistics show no paging. 256 MB of virtual
memory per process. Host memory is committed lazily by the OS, so an untouched virtual range costs nothing on iOS. If
a game's working set ever outgrows the phone, real paging (decommit on `sys_vm_store`/`invalidate`) is the lever.

## D-039 · 2026-10-03 · cellSheap is native with assumed semantics; only functions a log shows

Run 008. Sony's shared heap (cellSheap) lives in libsre and RPCS3 has no implementation (UNIMPLEMENTED stubs that
return CELL_OK), so there is no reference for its behaviour or its guest-memory layout. `cellSheapInitialize` keeps
the heap on the host and writes nothing into the guest range; `cellSheapAllocate` / `cellSheapFree` are left
unimplemented until a boot log shows them called, so their exact signatures come from the game's own calls rather
than a guess. To be checked against libsre once the firmware file is available (D-037). Also from run 008: SPURS
event flag initialize and workload shutdown follow RPCS3; natively no SPU runs a workload yet, so a shutdown makes it
removable at once and the completion hook (unused by GTA V so far) is logged, not called.

## D-040 · 2026-10-03 · SPURS SPU ports live in the native instance; event flags follow RPCS3 with two fixes

Run 011. On the PS3, attaching an lv2 event queue to SPURS (`cellSpursAttachLv2EventQueue`, and the queue an event
flag gets in `cellSpursEventFlagAttachLv2EventQueue`) connects it to an SPU port on every thread of SPURS' SPU thread
group. A native SPURS has no thread group, so each SPURS instance keeps its own port → queue map, with lv2's rules: a
given port 0..0x3F (0..0xF from SDK 1.8), or the lowest free one of 0x10..0x3F, and a port whose queue was destroyed
is free again. The native scheduler will send SPU-side events (an SPU task setting an event flag, a SPURS queue push)
through this map. Event flag set and wait run as one reservation operation on the flag's 128-byte line, so they stay
atomic against SPU code using GETLLAR/PUTLLC on it. Two departures from RPCS3, both bugs there: a blocked
`cellSpursEventFlagWait` returns the bits that woke it, and detaching a queue also disconnects its port. Waking SPU
tasks that wait on a flag needs the native taskset policy; until it exists that case stops with a fatal log instead of
being skipped.

## D-041 · 2026-10-03 · SPURS task creation: assumed LS pattern semantics, our own task attribute layout

Run 012. GTA V creates SPU tasks with `cellSpursTaskGetLoadableSegmentPattern`, `...GetReadOnlyAreaPattern`,
`...GenerateLsPattern`, `...GetContextSaveAreaSize`, `_cellSpursTaskAttributeInitialize` and
`cellSpursCreateTask[WithAttribute]`. RPCS3 implements only `cellSpursCreateTask`; the rest are stubs, so their
semantics are taken from what RPCS3's taskset code does with their results:

- An LS pattern has one bit per 2 KB block of local store, block 0 the top bit; the six blocks below 0x3000 belong to
  SPURS and may not be set (RPCS3 `_spurs::create_task`).
- The loadable-segment pattern is every block a PT_LOAD segment touches. The read-only pattern is only the blocks that
  read-only segments cover completely: when a task resumes, its read-only segments are reloaded from the ELF and the
  pattern's blocks are restored from the context (RPCS3 `spursTasksetLoadElf`), so a block shared with writable data
  has to be treated as writable.
- `GenerateLsPattern(start, size)` wants 2 KB alignment and a range inside 0x3000..0x40000.
- The context save area is 1 KB of execution context plus 2 KB per pattern block, the inverse of RPCS3's
  `(size - 0x400) >> 11`.

Every one of these logs "[native, assumed semantics]" so a boot log shows what the game asked for. They are checked
against libsre once the firmware reference is set up (D-037). `CellSpursTaskAttribute` is opaque (256 bytes), so
pspocket stores its own fields there. A created task is marked pending-ready and its taskset's workload is signalled,
but nothing runs it until the native taskset policy exists (stage 3b).

## D-042 · 2026-10-04 · RPCS3 runs Sony's libsre by default; HLE functions check the pointers they read

Run 013. RPCS3 does not use its own cellSpurs code for games by default: `g_prx_list` in
`rpcs3/Emu/Cell/lv2/sys_prx.cpp` marks `libsre.sprx` as not HLE, so libsysmodule loads the firmware's libsre and its
PPU code runs as it is. RPCS3's cellSpurs.cpp is therefore a partial, rarely exercised reference (it is how so many of
its task functions can be stubs). Consequences:

- libsre is the authority for the PPU side of SPURS as well, not only for job chains and queues (D-037). What was taken
  from RPCS3's HLE (initialize, workloads, tasksets, event flags, job chain attributes, D-040, D-041) gets checked
  against it once the firmware reference is set up.
- An HLE function checks a guest pointer before it reads it (`guest_readable`). A wrong guess about a firmware
  signature then gives a logged error, not a host crash: the run goes on and the log shows what the game passed.
- Those errors, unimplemented calls and `pspocket-boot`'s crash report show r3..r10, and the crash report and
  `log_call_site` disassemble the caller's last 16 instructions, so one run shows what the game put in each register.
- From run 013: a task's LS pattern is read only when the task has a context. The pattern only describes what the
  context keeps, and GTA V's first `_cellSpursTaskAttributeInitialize` passed 0x40 in one of those two registers.

## D-043 · 2026-10-04 · The game reads its disc image in place; boot.sh finds it or copies it from the iPhone

Run 014. GTA V's first file access (`cellFsStat` and `cellFsOpen` on `/dev_bdvd/...`) needs the disc. That settles
the open question of whether to extract the disc or read the ISO: **read it in place**. lv2 gets a read-only
`VfsImage` interface, and the loader puts runtime/disc's ISO 9660 reader behind it. `/dev_bdvd` mounts the image root
and `/app_home` mounts `PS3_GAME/USRDIR`, where RPCS3 puts `/app_home` for a game started from the drive. Every sys_fs
syscall reads image mounts with the results a read-only host directory gives. Lookups ignore case, as the reader does
on PS3 discs, and files over 4 GB read across their extents. Nothing is extracted, so the 18.7 GB GTA V image costs no
extra space, and the iOS app can use the same path on the copy it already holds.

On the Mac, `scripts/boot.sh` takes the first image that reads as a PS3 game disc (`pspocket-boot --probe-disc`),
searching in this order:
1. `$PSPOCKET_ISO`;
2. `inventory/<game>/`;
3. the Desktop and Downloads.

If none of those has one, it copies the image the app holds on the connected iPhone into `inventory/<game>/disc.iso`
once (`scripts/ios.sh pull-iso`, through devicectl). That image, like everything in `inventory/`, never goes into git.
The game's `/dev_hdd0` is `inventory/<game>/hdd0`, for game data and saves.

## D-044 · 2026-10-04 · cellFs over lv2, cellGame as RPCS3 without the delays, sceNp2Init offline

Run 014.

- **cellFs.** The 20 `cellFs` functions GTA V imports forward to lv2 as RPCS3's `cellFs.cpp` does. Read and write at
  an offset go through `sys_fs_fcntl` 0x8000000A/B with RPCS3's argument block, and allocate-area goes through
  0xE0000017. lv2 gains `sys_fs_fcntl`, `sys_fs_chmod` and `sys_fs_utime`. Deviation: an fcntl operation lv2 doesn't
  implement logs and returns `CELL_EINVAL`, where RPCS3 returns `CELL_OK` (rule 4).
- **cellGame.** It follows RPCS3's `content_permission`: a check opens a session and ContentPermit ends it. Game data
  is created in a temporary directory and moved into place with a PARAM.SFO written by a PSF writer that follows
  RPCS3's `save_object`. RPCS3's sleeps, which imitate the PS3's slow calls, are left out. The error dialog only logs,
  since there is no UI yet.
- **sceNp2Init.** It brings NP up with no user signed in, so the status reads offline.

## D-045 · 2026-10-04 · The RSX lives in lv2; cellGcmSys is HLE on top of it, as in RPCS3

Runs 015 and 016. GTA V's `_cellGcmInitBody` is the first graphics call, so the RSX front end (D-032) gets wired to a
game for the first time:

- **lv2 owns the RSX** (`runtime/lv2/src/rsx.cpp`, RPCS3 `sys_rsx.cpp`). `sys_rsx_memory_allocate` commits local memory
  at 0xC0000000. `sys_rsx_device_map` reserves the rsx_context area through the same first-free-256 MB allocator as
  user memory, as RPCS3's `vm::reserve_map` does (GTA V: 0x40000000). `sys_rsx_context_allocate` fills DMA control,
  driver info and reports as RPCS3 does, then creates and starts `rsx::Rsx`. The kernel config's `rsx_backend`
  factory supplies the backend; without one, the new `rsx::NullBackend` runs every command and draws nothing, which is
  what `pspocket-boot` uses on the Mac. Unhandled RSX methods abort (rule 5) unless `rsx_log_failures` is set.
- **These are C++ entry points for HLE, not bound syscalls.** No game has called syscalls 666-677 directly (rule 4).
- **cellGcmSys** (`runtime/hle/src/cellGcmSys.cpp`): the 20 functions GTA V imports, following RPCS3. The
  CellGcmContextData sits at the start of the device area, and the offset tables, display info, tiles and Z-cull
  arrays sit in the main area, all where RPCS3 puts them. Display buffers, tiles and Z-cull go through
  `sys_rsx_context_attribute` (0x104, 0x300, 0x301) the way the firmware's libgcm does. RPCS3's HLE writes them into
  its renderer directly instead.
- **Internal HLE functions.** `cellGcmCallback`, which the game reaches through the context when a 32 KB segment is
  full, has to have a guest address without being an import. A registry flag (`PS_HLE_INTERNAL`) makes the loader
  link such functions for every imported module before guest threads start, because the dispatch table can't grow
  later. HLE looks them up with `HleProcess::descriptor_of`.
- **HLE can call the guest** (`call_guest` in `guest.hpp`). It gives the callee a fresh frame below r1, because the ABI
  lets a callee write its caller's LR, TOC and parameter save slots, and a `StackVar` sits at r1.
- **Vblank.** `_gcm_intr_thread` is an lv2 PPU thread whose entry is an internal HLE function. Every 1/59.94 s it counts
  a vblank in the driver info (package 0xFED, RPCS3's vblank hack) and calls the game's vblank handler with head 1.
  Flip and user handlers come when a game imports their setters.

## D-046 · 2026-10-04 · The firmware reference is decrypted on the Mac by a local script, never committed

D-037 and D-042 need Sony's libsre as the reference for SPURS. RPCS3 isn't installed on the Mac, but its source is
(`../ps3-ios-engine/external/rpcs3`). A small script follows RPCS3's own code: the PUP reader (`Loader/PUP.cpp`, entry
0x300 is update_files.tar), the firmware installer (`main_window.cpp`: each `dev_flash_*` package is an SCE container
whose third section is a tar), and `SELFDecrypter` for the SPRX. It reads the keys from RPCS3's `key_vault` source at
run time. Script and outputs stay in `inventory/firmware/` (gitignored): the script in `tools/fw_unself.py`, the ELFs
in `elf/`. From firmware 4.92 we have `libsre.prx.elf` (cellSync, cellSheap, cellSpurs, cellDaisy, ...) with its two
embedded SPU kernels carved out, and `libspurs_jq.prx.elf`. libsre also carries the SPU policy modules as raw code
blocks, the taskset module at LS 0xA00. Reading it uses scratch disassemblers built on the repo's PPU and SPU
disassemblers. Nothing from the firmware, and no disassembly of it, goes into the repository. Code comments describe
the behaviour they follow ("as libsre").

## D-047 · 2026-10-04 · SPURS tasks, taskset shutdown and cellSheap follow libsre (fw 4.92)

Run 015 (D-046 made the check possible). Changes from D-039, D-041 and D-042:

- **`_cellSpursTaskAttributeInitialize(attr, revision, sdk, elf, saveConfig, argument)`** takes six arguments. The
  fifth is a `CellSpursTaskSaveConfig {u32 context; u32 contextSize; u32 lsPattern*}`. This explains run 013: Bink
  passed a stack save config and no argument, and garbage sat in r9/r10. The attribute now uses libsre's layout, which
  replaces D-041's own:

  | Offset | Field |
  |---|---|
  | 0x08 | ELF (u64) |
  | 0x10 | context (u64) |
  | 0x18 | size |
  | 0x20 | pattern |
  | 0x30 | argument |
  | 0x40 | exit code container |

  The attribute only needs to be 8-byte aligned.
- **Task creation** follows libsre's check order and now checks the ELF. Its ident must be `7F 45 4C 46 01 02 01 00`,
  with EM_SPU, and the entry and every PT_LOAD address must be at least 0x3000, else NOEXEC. Contexts need 128-byte
  alignment for process SDK 0x280000 and up. A wake-up STAT becomes TASK STAT.
- **LS pattern helpers.** The D-041 semantics are confirmed: one bit per 2 KB block, six reserved, and a save area of
  1 KB + 2 KB per block. Corrections:
  - Only PT_LOAD segments with paddr below 0x40000 count.
  - Any program header past 0x40000 is INVAL.
  - The read-only pattern comes from `p_filesz`, not `p_memsz`.
  - `GenerateLsPattern` has no lower bound at 0x3000.
  - `GetContextSaveAreaSize` checks no pointers.

  GTA V's patterns are unchanged.
- **Taskset shutdown.** `cellSpursShutdownTaskset` refuses only workload IDs above 31 and passes the workload
  shutdown's result through (STAT becomes TASK STAT). `cellSpursShutdownWorkload` gains SRCH for unregistered
  workloads.
- **cellSheap** (supersedes D-039) is libsre's buddy allocator in guest memory: a 128-byte header, a bitmap with 2 bits
  per tree node, and 128-byte blocks. Allocate returns 0 on failure. Free merges buddies. A double free is a libsre
  assertion.
- **libsre's assertions.** Where libsre asserts (it prints the condition and aborts the process), pspocket stops with a
  fatal log naming it. Where libsre would read a bad pointer, pspocket refuses the call with a logged error, as in
  D-042.

## D-048 · 2026-10-04 · cellSysutil reports RPCS3's default console; the system cache lives next to hdd0

Runs 015 to 017. GTA V calls these functions right before and after GCM init. Implemented now: the 16 cellSysutil
functions it imports (callbacks, system parameters, system cache, video and audio output state) and the two
cellSysutilAvconfExt ones. Not implemented yet: dialogs, the on-screen keyboard, save data and BGM.

- **The console reported** is RPCS3's default configuration, which suits a fixed iPhone target:
  - display: one HDMI display, 1280x720 progressive at 59.94 Hz, 16:9, RGB;
  - sound: stereo LPCM at 48 kHz;
  - settings: English, enter on cross, parental control off, no PSN account (D-024);
  - two values differ from RPCS3: the nickname "PSPocket", and the user name, which comes from the host's
    `/dev_hdd0/home/00000001/localusername` when that file exists, else "User".
- **System events** go through the callbacks the game registers. `sysutil.hpp` queues events for other HLE code, and
  `cellSysutilCheckCallback` calls the callbacks on the game's thread through `call_guest` (D-045). Nothing sends an
  event yet: there is no system menu.
- **The system cache** follows RPCS3: `/dev_hdd1` is mounted on `caches/<TITLE_ID>_<cacheId>` under the hdd1 root.
  That root is the host's `/dev_hdd1` mount if there is one, else `hdd1` next to the host directory of `/dev_hdd0`,
  which is `inventory/<game>/hdd1` with `scripts/boot.sh`. RPCS3's relay and clear rules apply, with its in-use
  marker, which pspocket writes in every case, not only for a new cache.

## D-049 · 2026-10-04 · Input: one virtual controller the host feeds; no keyboard

Runs 018 and 019. `sys_io` (the 14 cellPad and cellKb functions GTA V imports, following RPCS3) reports one
controller, always connected, on port 0. It looks like RPCS3's DualShock 3: standard type, press-sensitive buttons,
motion sensors, high-precision sticks, rumble. The host sets its state with `hle::set_pad_input`
(`pspocket/hle/input.hpp`). The iOS app will drive it from a touch overlay or an MFi/DualSense controller. On the Mac
it stays at rest. A connected controller is the right default for an app that always shows a virtual one; games stop at
"connect a controller" otherwise.
- `cellPadGetData` reports changes the way RPCS3 does: the first read, any change, and every 10 ms with sensors on.
  Rumble is accepted and ignored for now.
- RPCS3's sys_config event thread is not reproduced: the controller counts as connected, assignment change included,
  from `cellPadInit`.
- No keyboard is connected, as with RPCS3's default null keyboard handler.

This settles how input reaches the game, not the open question of whether a hardware controller is required.

## D-050 · 2026-10-04 · Native SPURS runs SPU tasks: host workers and a taskset policy that follows libsre

Runs 020 and 021 (D-037 stage 3b). Each SPURS instance gets one host worker per SPU (`spurs_scheduler.cpp`). A worker
is a pthread with an SPU context and 256 KB of local store.
- **Selection.** A worker picks a workload by the SPURS kernel's rules (RPCS3 `spursKernel1SelectWorkload`), using the
  guest CellSpurs line 0 (ready counts, signals, the workload flag, contention). It updates that line through the
  reservation system.
- **Idle.** An idle worker sleeps in `ps_res_wait_change` on that line: any ready count, signal or flag change from PPU
  HLE, SPU code (PUTLLC) or another worker wakes it, which is what the SPURS kernel's idle loop waits on too.
- **Not reproduced.** The system service never runs: its message is acknowledged, and workload state is read directly.
  Pending contention is not tracked, and SPURS2 has no scheduler yet.

**The taskset policy** (`spurs_taskset.cpp`) follows libsre's taskset policy module, read from the firmware, field by
field. RPCS3's `cellSpursSpu.cpp` is an unwired draft that differs in several places (sparse context blocks, LS cleared
at every start, exit-handler sizes, the receive-flag syscall).
- What task code sees is libsre's: the kernel context at LS 0x100 and the taskset context at 0x2700, the task image
  with libsre's ELF-loader checks, and the registers at entry.
- The syscall entry at 0xA70 is a `stop` the worker takes: 0 exit, 1 yield, 2 wait for a signal, 3 poll, 4 receive the
  workload flag.
- The task bits change on the guest taskset line with libsre's state machine, and ready counts are updated after every
  request.
- A task that yields or waits is saved to its own context save area exactly as libsre saves it: 0x380 bytes, then the
  pattern's blocks, packed. It resumes on any worker with $3 = 0.

**Waking tasks from the PPU.** `cellSpursEventFlagSet` writes each released task's events and sends it a signal
(`_cellSpursSendSignal`), replacing D-040's deliberate fatal stop.

**Events to the PPU.** SPU code's interrupt-mailbox events (`sys_spu_thread_throw_event` / `send_event`) reach lv2
event queues through the instance's SPU port map (D-040).

**SPU runtime hooks.** Contexts can have their own stop and interrupt-mailbox handlers (`SPUContext::hooks`), so SPURS
workers don't take over the process-wide ones, which lv2's SPU threads will need.

**Tests and a bug found.** The tests run hand-assembled SPU tasks that exit, wait and resume, get woken by an event
flag, throw events, and yield to each other. They found a lost-update bug in the ready-count update: the running total
was changed inside a retried reservation update, so a retry after another writer's store dropped the increment. A
regression test forces those retries.

## D-051 · 2026-10-04 · Custom workloads: a game's own policy module runs on a worker

Run 021. GTA V adds a workload with its own SPU policy module (`cellSpursAddWorkload`: 0x11B0 bytes, priority 12 on
SPUs 0-3, contention 5). Since it is game code, the worker runs it rather than imitating it.
- **Loading and entry.** The image is loaded at LS 0xA00, and reloaded only when another module was there, as libsre's
  kernel does. It is entered with the kernel's registers: $0 = 0x804, $1 = 0x3FFB0, $3 = 0x100, $4 = its argument,
  $5 = the poll status.
- **Kernel entry points.** The kernel's entry points it can reach are `stop`s the worker answers: select (0x290, via
  `[0x1E4]`), its return (0x804) and exit (0x808, via `[0x1E0]`).
- **Work arrives** through `cellSpursReadyCountStore` and the workload flag (`_cellSpursWorkloadFlagReceiver`,
  `cellSpursGetWorkloadFlag`), both checked against libsre.
- **Offset fix.** The workload flag word is at CellSpurs 0x6C, inside the 16-byte CellSpursWorkloadFlag at 0x60; it
  was read at 0x60 before.

## D-052 · 2026-10-04 · cellAudio: RPCS3's ports and periods on a host thread; output through a host sink

Run 021. The 11 cellAudio functions GTA V imports follow upstream RPCS3:
- 8 ports (4 for SDK ≤ 3.5) from RPCS3's free-list stacks, with its checks and error codes;
- notify queues on keys from 0x80004D494F323221, with one CELL_AUDIO_EVENT_MIX event per period.

How pspocket does it:
- **Memory.** The read indices (big-endian u64 in a 64 KB area, 16 bytes per server index) and the port buffers are
  main-area allocations. RPCS3 maps lv2 shared memory instead.
- **Thread.** The audio thread is a host thread that runs no guest code, from `cellAudioInit` to `cellAudioQuit`; it
  also stops when the process exits. Every 256/48000 s it clears the block just played, advances the read index and
  notifies the queues. When it falls more than 100 ms behind, it skips the missed periods rather than bursting.
- **Output.** There is no output backend yet. `hle::set_audio_sink` (`pspocket/hle/audio.hpp`) gets RPCS3's stereo mix
  each period, and the iOS app will attach CoreAudio there.
- **Gap.** lv2 can't look a queue up by its IPC key yet, so only queues made by `cellAudioCreateNotifyEventQueue` can
  be registered. GTA V uses that one.

## Open decisions (plan section 14)

- [x] Licence → D-001.
- [x] Game version → D-025: GTA V BLES01807 (GTA IV in D-002 stays as a draft).
- [x] Toolchain → D-012 (write our own).
- [x] SPURS → D-037: native (HLE module + host scheduler). libsre from the user's firmware is a reference only, for job
  chains and queues.
- [~] Target devices: primary test device is D-011; minimum chip and the iPad question are still open.
- [ ] Input: controller required, or a polished touch overlay as well.
- [x] ISO handling → D-043 (read directly from the ISO).
