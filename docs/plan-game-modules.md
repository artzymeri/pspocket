# Plan: a game's own PRX modules and its disc SPU programs, translated when preparing

2026-10-07. Written by a design workflow (four readers mapped the loader, the system library, preparation and SPU
discovery; three independent designs; three judges; a synthesis). Read-only analysis of the code as of d2fbe8a; each
commit below gets its own D-number when it lands (the numbers here are provisional) and is checked as it goes.

PLAN: translate a game's own PRX modules and its disc SPU programs at preparation time

Base design: "minimal". Two of the three judges ranked it best, and it averages correctness 7.5 and effort 7 against 6.5/4 for "reuse" and 6.7/3 for "robust". Its basic shape stays:
- a second per-game "modules library" built the way prepare_system builds the system library;
- no change to the library ABI, so every cached object keeps its key (cache_key includes ps_game_abi_hash, runtime/prepare/src/prepare.cpp:402);
- static SPU programs from the EBOOT come first.

The judges' grafts are folded in where they fit. Everything here comes from reading files only. Nothing was built or run.

FACTS CHECKED NOW (read-only), which the plan relies on

1. prx_dispatch_enable appends without removing duplicates and calls PS_FATAL when the range table is full (runtime/ppu/src/runtime.cpp:144-152). Any design that enables a range on every load needs this fixed first.
2. prx_fixed_layout returns {} as soon as one segment overflows (recomp/common/src/prx_place.cpp:42). The firmware layout uses the whole PRX region (runtime/loader/src/lle.cpp:386-400).
3. lift_spu_images reads one folder (runtime/prepare/src/spu.cpp:56). It deletes every program folder it did not lift in the current pass (spu.cpp:192-194).
4. library_is_current checks kTranslatorGeneration (prepare.cpp:207-210), and system_library_is_current calls it (runtime/prepare/src/system.cpp:345). Raising the translator generation therefore re-prepares the system library too.
5. The lifter writes one shared header holding every prototype, and splits units by total/tus (recomp/ppu/src/lifter.cpp:1138-1160). headers_define hashes every .h in the folder (prepare.cpp:61-75). Any new function therefore recompiles every unit in that folder.
6. Only executable segments are scanned for SPU ELFs today. GTA V's EBOOT holds 8 SPU ELFs, 506,168 bytes (inventory/gta5/eboot.json:3375-3377). Assassin's Creed's holds 6, 437,356 bytes, including mstream_dsp_reverb.pic (inventory/BLES00158/eboot.json:1695-1697).
   - D-154's "264 of 265" counts raw job binaries, which no ELF scan finds.
   - AC's tasks at 0x1026ac80 and 0x1007a980 are EBOOT-embedded ELFs (hkspursintegrate, LinearCast), not data files.
7. Every import library of GTA V and AC has attributes 0x9 (23 and 10 libraries), with no 0x2000 function refs and no imported variables. Link-time code patching (runtime/loader/src/loader.cpp:46-55, 314, 322) does not affect them.
8. Sniper: Ghost Warrior loads engine_jobs.sprx at 0x2530000, with 20 relocations and 0 exports (inventory/phone/logs/20261006-110909-game-prev.log:160-162). It is the one local title that exercises the game-PRX path. GTA V and AC load no module of their own.
9. ps_spu_find_program matches a relocatable program at any code_start (runtime/spu/src/runtime.cpp:226-232). SPURS rejects a task whose entry is 0x2FFF or lower (runtime/hle/src/spurs_taskset.cpp:243). So an SPU ELF whose main segment sits at vaddr 0 is not a task, and saving it as kind "module" also serves task lookups.
10. For a plain ELF loaded by fd at an offset, load_at keeps the archive bytes that follow it (runtime/loader/src/lle.cpp:867-873). A module key must not hash those bytes.
11. load_prx places a module and then links it, with no hook in between (lle.cpp:1044-1060). SystemLibrary::check runs after linking (runtime/loader/src/system.cpp:63-81, fired from lle.cpp:757).
12. Translating every firmware SPU program measured as most of an hour on the phone (docs/decisions.md:4128-4130, D-191). Deep scans must stay off by default.
13. The latest decision is D-225 (decisions.md:4958), so new ones start at D-226. The latest run note is 172.

ORDER OF WORK

Measurement comes first (rule 8), then the cheapest high-value change (static SPU), then the PRX pipeline on the Mac (rule 2), then the phone wiring, then the disc walk, then the PPU feedback loop. Each commit is small, for one component (rule 9), with its own D-number. Tests are synthetic only (rules 1 and 7).

Phone checks are batched at the end: memory says no phone tests until everything is implemented, and rule 10 says ask the user. Mac checks use build/host only, because the disk is nearly full.

COMMIT 1 (D-226) Attribute interpreted PPU code to its owner

What:
- A new loader::attribute_pc(addr) returns one of: "exe", "game module <name> <identity>", "fw <key>" or "other". It uses RuntimeModules::id_by_address (lle.cpp:993-1003), LleModules' fixed addresses and the executable's segments.
- pspocket-boot's D-207 report (runtime/loader/tools/boot_main.cpp:543-551, 1348-1355) prints the interpreted steps per bucket and labels the top 12 sites.
- The app logs the same report every 30 s and at exit (runtime/platform/ios/src/game.mm:377-391). Today it never reads ps_ppu_interp_sites.

Files: runtime/loader/include/pspocket/loader/lle.hpp, runtime/loader/src/lle.cpp, boot_main.cpp, game.mm, docs/decisions.md.

Synthetic test (tests/hle/test_lle.cpp, beside the D-159 fixture at :280):
- A debug-SELF PRX is loaded from a fake /app_home and its function is called under PS_PPU_MISS_INTERPRET.
- All of ps_ppu_interp_sites' steps fall under "game module"; the exe and other buckets are 0.

GTA V / AC: behaviour does not change. Record the baseline per bucket in a run note: GTA V's exe/fw split and AC's (its SPU line reads 184 instructions, docs/compatibility.md:20). scripts/gate.sh (D-149) must show unchanged fps.

COMMIT 2 (D-227) Decode import reference tables, add a census and a dev guard against link-patched code

What:
- ppu_module decodes function reference tables (today only their address is kept, recomp/common/src/ppu_module.cpp:171-174) into entries, as it already does for variables (:186-201).
- A new helper, refs_into_code(module, elf), lists ref entries whose target lies in an executable PT_LOAD.
- A shared USRDIR walk is extracted from collect_modules (runtime/disc/src/extract.cpp:86-103) into runtime/disc/src/walk.cpp plus include/pspocket/disc/walk.hpp. It reads 16 bytes of magic per file.
- ps3inspect gets --disc-census IMAGE.iso|DIR. Per file it reports:
  - kind: PPU PRX, PPU executable, SPU ELF/SELF, SDATA, EDATA or other;
  - SELF type and NPDRM licence type;
  - PT_LOAD sizes;
  - refs into code (the EBOOT's included);
  - the number of embedded SPU ELFs.
  Data files and SDATA are scanned only with --deep (scanned in chunks with overlap).
- LleModules::lay_out logs "firmware layout ends at X (N MB)" and gains layout_end(). The judges computed 0x32E50220, 46.3 MB, from inventory/firmware/installed/lle-index.txt.
- Dev guard (rule 5), in the EBOOT linker (loader.cpp:314, 322) and in RuntimeModules::start's repointing (lle.cpp:914-949): when a translated image is loaded and patch_refs targets one of its executable segments, PS_FATAL in dev builds, PS_LOG_ERROR in release. Translated code would otherwise keep the immediates it was lifted with from the file bytes (prepare.cpp:284).

Files: recomp/common/src/ppu_module.cpp, recomp/common/include/pspocket/recomp/ppu_module.hpp, runtime/disc (walk), tools/ps3inspect, runtime/loader/src/loader.cpp, lle.cpp, lle.hpp, tests/common/iso_builder.hpp (IsoBuilder moved out of tests/disc/test_disc.cpp:57).

Synthetic tests:
- A synthetic ISO holds:
  - EBOOT.BIN (build_ppu_exec);
  - a debug-SELF PRX whose variable ref table targets a lis/lwz pair;
  - an NPDRM SELF;
  - a standalone SPU ELF;
  - a data file with an SPU ELF across a chunk boundary;
  - an SDATA file (build_sdata).
- The census reports 1 PRX, 1 that patches code and 1 NPDRM, and finds the SPU ELF in the data file only with --deep.
- The guard predicate fires for the patched exe. test_lle: layout_end() equals the base plus the 64 KB-rounded sizes.

GTA V / AC: run the census on the local ISOs (output to inventory/, counts only in docs/runs). Expect:
- 0 refs into code;
- 8 and 6 SPU ELFs in the EBOOTs;
- the firmware end at about 46.3 MB.
Run it on Sniper as well. A gate.sh run with the guard compiled in must not trip.

COMMIT 3 (D-228) Delete stale objects

What: after a successful link, codegen::build_library removes the objects in object_dir that this build neither compiled nor reused. Today none are ever deleted (runtime/codegen/src/build.cpp:134-186). This lands before any commit that causes a re-preparation, which matters with the disk nearly full.

Files: runtime/codegen/src/build.cpp, runtime/codegen/include.

Synthetic test (tests/codegen): build, change one source, build again. The old object is gone, the unchanged object is kept, and the library links.

GTA V / AC: run pspocket-prepare twice. The second run keeps every file and the objects/ size is unchanged. Record the bytes freed in an old game_dir.

COMMIT 4 (D-229) Static SPU programs from the EBOOT; several input folders for the SPU lift

What:
- save_spu_program and save_spu_images move from system.cpp:58-131 into runtime/prepare/src/spu_static.cpp and take generic {addr, bytes} segments. The system path's output stays byte-identical.
- PIC rule, on for games only: when the main segment's vaddr is 0, save it as "kind module" so it is lifted relocatable (fact 9). It is off for the system path until a kSystemGeneration bump has another reason.
- In prepare_game step 1b (prepare.cpp:310-328), clear game_dir/spu-static, scan the EBOOT's PT_LOADs at their vaddr, then lift from {coverage, spu-static}.
- lift_spu_images takes a vector of folders:
  - each .bin is read from the folder its .txt came from (today it is read from the single dump_dir, spu.cpp:154);
  - the seed lines of images with the same name are merged across folders, rather than the first folder winning;
  - pruning covers the union of folders.
- Inputs generation: a new kGameInputsGeneration = 1 and a Manifest.inputs_generation field (missing reads as 0).
  - Only game currency checks it: library_is_current gains a minimum that the game path passes and system_library_is_current does not.
  - So existing game libraries re-prepare once, and the system library does not (fact 4). Any change to what prepare feeds the lifters raises this number, because the translator hash does not cover runtime/prepare (runtime/game/CMakeLists.txt:21-26).
- The first time estimate counts the static images (prepare.cpp:264-276, 318).
- Fix the D-204 to D-205 citation at spu.cpp:97 and spu.hpp:19.

Files: runtime/prepare/src/spu_static.cpp and include (new), system.cpp, prepare.cpp, prepare.hpp, spu.cpp, spu.hpp, CMakeLists.txt.

Synthetic tests (tests/game/test_prepare_spu.cpp):
- build_ppu_exec carrying build_spu_elf (two executable segments) is prepared with an empty coverage folder. It gives spu_<hash>_<vaddr> for both segments, under the key Policy::attach_code computes (spurs_taskset.cpp:270-295). After load_library, ps_spu_find_program finds it and running it adds no ps_spu_miss_count.
- A vaddr-0 SPU ELF is saved as kind module and found by a task lookup at another LS address.
- A coverage image with the same name plus "seed" lines lifts once, with the seeds from both folders.
- A coverage-only program survives next to the static ones.
- test_system_library passes unchanged.

GTA V / AC:
- Prepare each from an empty coverage folder, before and after. Expect +8 and +6 programs; the PPU objects are all kept (D-209).
- pspocket-boot --game-library ... --spu-dump EMPTY --timeout 120, over the same walk, A/B in one session under caffeinate. Session 1 should no longer dump or interpret collision, forcesolver, inflateServer or bink (GTA V), or Havok, LinearCast and ATRAC (AC). GTA V's raw EDGE jobs remain.
- gate.sh fps unchanged. The system library still reports "ready".

COMMIT 5 (D-230) Make enabling a translated range idempotent

What: prx_dispatch_enable returns without adding anything when [lo, hi) is already inside an enabled range. This also protects the system library if a firmware module is ever placed again.

Files: runtime/ppu/src/runtime.cpp:144-152, tests/ppu/test_dispatch.cpp.

Synthetic test: enabling the same range 2,000 times, and a sub-range of it, leaves prx_dispatch_enabled_ranges() at 1. Lookups are unchanged.

GTA V / AC: the system library's "runs translated" count and enabled ranges are unchanged in a pspocket-boot run. gate.sh passes.

COMMIT 6 (D-231) Module identity and the game-module layout

What:
- recomp::prx_identity(ElfFile, bytes) is the SHA-256 over the ELF header, the program header table and each phdr's file range, in order. Trailing archive bytes and the section table don't change it, while code or data does: identity covers data segments too, so no separate data check is needed. This is minimal's design, with reuse's trim.
- recomp::game_module_layout(previous slots, new modules' sizes, floor):
  - top-down from PS_MEM_PRX_BASE + PS_MEM_PRX_SIZE (0x40000000), robust's design;
  - each module is a block laid out internally with prx_fixed_layout's 64 KB rules;
  - append-only: existing slots never move;
  - a partial fit leaves a module that doesn't fit without a slot while the others keep theirs (reuse's rule);
  - it refuses to go below the floor.
- There is no 128 MB firmware cap: lay_out keeps PS_MEM_PRX_SIZE, so firmware addresses, identity_text and every installed system library are unchanged.
- Text format, shared like firmware_identity_line (recomp/common/src/firmware.cpp:203-208):
  - 'E <eboot sha256>';
  - 'F <floor>';
  - 'G <identity> <source> <addr>...'.
  There is a writer and a parser.

Files: recomp/common/include/pspocket/recomp/prx_place.hpp, prx_place.cpp, firmware.cpp, tests/common/test_prx_place.cpp.

Synthetic tests: identity is equal for a synthetic PRX, the same PRX followed by 4 KB of junk, and the same PRX decrypted from its debug SELF; it differs when one code byte changes. The layout:
- is deterministic;
- appending a module never moves earlier ones;
- with an oversized middle module, earlier modules keep their slots and later ones are still placed;
- keeps every address in [floor, 0x40000000) and 64 KB aligned;
- round-trips through the text format.
The D-178 layout tests are unchanged.

GTA V / AC: no run-time effect. The system library's identity hash is unchanged (it is still current).

COMMIT 7 (D-232) RuntimeModules places a prepared module at its slot, checks it before linking, and captures unprepared modules

What:
- RuntimeModules gains set_game_layout(map identity -> addrs, floor), set_hooks(on_placed, on_linked) and set_capture_dir(dir). PrxModule gains its identity.
- The game branch of load_at (lle.cpp:864-907) computes prx_identity after decryption. It places the module at its slot (place() already takes fixed addresses, lle.cpp:117-150) when:
  - the layout has the identity;
  - no live (not Unloaded) entry holds that range;
  - the slot is at or above lle->layout_end().
  Otherwise the module goes to main memory as today, and the log says why.
- load_prx gains an on_placed callback between place and link_imports, so the code hash is taken before linking: stub slots in a single read-write-execute segment don't break it, and the lifted stubs read the slots from memory.
- patch_refs gains an optional guard range and reports any write into the module's code. on_linked receives that list.
- On a miss with a capture dir set, the module is saved once as <identity>.elf: the decrypted ELF trimmed to max(segment file ends, section table end), so .opd and section lookups still work. A .txt beside it records the path, offset and name. The log says "not prepared: saved for the next preparation".
- pspocket-boot gets --prx-dump DIR.

Files: runtime/loader/include/pspocket/loader/lle.hpp, runtime/loader/src/lle.cpp, runtime/loader/src/loader.cpp (patch_refs guard), runtime/loader/src/refs.hpp, boot_main.cpp.

Synthetic tests (tests/hle/test_lle.cpp, D-159 fixtures :280-331, by-fd case :334):
- With a layout naming the module, segment 0 lands at its slot, on_placed fires once before linking, and module_start still writes its marker.
- Without a layout it goes to main memory and the capture dir gets one .elf; a second load writes nothing.
- A second live load goes to main memory.
- Stop, unload and load again returns the same slot.
- Loaded by fd at offset 0x800 inside an archive, it gets the same identity and slot.
- An extra main_memory().alloc before the load doesn't change the address.
- A slot below the floor falls back to main memory.

GTA V / AC: no own modules, so no change. The D-159 tests and gate.sh pass. Sniper (Mac, if its ISO is local): its capture dir gets engine_jobs' .elf.

COMMIT 8 (D-233) Prepare the modules library, one lift per module

What: new runtime/prepare/src/modules.cpp, prepare_game_modules(ModulesOptions{module input dirs, eboot_sha, floor, out = game_dir/modules, toolchain, workers, key, progress, cancel}). It is separate from prepare_game, so a failure leaves the EBOOT library alone (reuse's isolation). It:
1. Decrypts the inputs with builtin_self_keys. NPDRM modules are counted and skipped.
2. De-duplicates by identity.
3. Reads the previous identity.txt and runs game_module_layout, appending new modules in source order.
4. Places each module with place_prx at its slot. Seeds come from relocated export descriptors plus .opd, through a helper factored out of system.cpp:203-252 that both passes share (it includes the place_of libpngdec overlap rule).
5. Skips and counts modules for which refs_into_code (commit 2) is non-empty.
6. Analyzes and lifts each module on its own into modules/ppu/<id16>/, with name mod_<id16> and its own header.
   - A generated modules/ppu/game_ppu_reg.c defines game_ppu_register (calling each module's register), game_ppu_function_count, and an empty game_ppu_image_words, which check_image_words=false never reads.
   - Job collection in prepare.cpp:338-342 also walks the immediate subfolders of ppu/, each with its own headers_define.
   - So a newly captured module compiles only its own units, the table file and the entry (this fixes fact 5 for the modules library without touching the lifter).
7. Writes modules.txt ('M <identity> addr size sha256' per code segment as placed, before linking) and identity.txt.
8. Sends each module's SPU ELFs to modules/spu-static through spu_static.
9. Calls prepare_game with executable = identity.txt, ppu_dir set, discover_spu = true and the inputs generation, the same scheme as prepare_system (system.cpp:290-340).

Also:
- If there are no inputs, a 'none' marker is written so the check passes quickly.
- game_modules_are_current(dir, eboot_sha, inputs, floor) requires all of:
  - library_is_current on SHA-256(identity.txt);
  - E == eboot_sha;
  - every captured identity is in identity.txt;
  - the F floor is at or above the current firmware end.
- pspocket-prepare gets --modules DIR and --system DEV_FLASH (for the floor).

Files: runtime/prepare/src/modules.cpp, module_translate.cpp (shared with system.cpp), include/pspocket/prepare/modules.hpp, prepare.cpp (subfolder jobs), system.cpp, runtime/prepare/tools/pspocket_prepare.cpp, tests/game/test_module_library.cpp (in the codegen-only block of tests/game/CMakeLists.txt, modelled on test_system_library.cpp:135-255).

Synthetic tests:
- A synthetic game PRX (build_ppu_prx; module_start stores a marker; an export returns 5; an SPU ELF with an overlay inside; a debug SELF) produces modules/game.dylib, identity.txt with its slot, modules.txt, and both SPU programs.
- A module whose ref table targets code is reported as skipped.
- Lifting twice gives byte-identical output.
- After a second module is captured, the first module's slot is unchanged and the result has files_compiled <= (the new module's units + 2) and files_kept > 0.

GTA V / AC: prepare_game_modules writes 'none'. The EBOOT library and the system library are untouched (same manifests).

COMMIT 9 (D-234) Load the modules library and gate each module (Mac)

What:
- loader::SystemLibrary is generalised into a ModuleLibrary whose check is keyed by any string (runtime/loader/src/system.cpp:25-81). SystemLibrary's interface stays on top.
- New loader::GameModules::load(dir, eboot_sha, RuntimeModules&, LleModules*, memory):
  1. parses identity.txt and refuses on an E mismatch;
  2. refuses when the lowest slot is below lle->layout_end(), because registration is "later wins" (runtime.cpp:78-85);
  3. calls game::load_library with executable_sha256 = SHA-256(identity.txt) and check_image_words=false;
  4. installs the layout and hooks into RuntimeModules.
- on_placed hashes the code segments against modules.txt. on_linked enables them (idempotent, commit 5) only on a match with no code write during linking. Otherwise it logs "runs under the interpreter".
- After module_start returns (RuntimeModules::start), the code is hashed again: PS_FATAL in dev, logged in release (rule 5). This catches code that changes itself.
- Wiring, after game::load_library and before any guest thread (prx_insert reallocates, runtime.cpp:70-87):
  - pspocket-boot: --modules-library DIR|none, defaulting to <game-library dir>/modules;
  - game.mm, after :467-476.

Files: runtime/loader/src/system.cpp, runtime/loader/include/pspocket/loader/system.hpp, new runtime/loader/src/game_modules.cpp and .hpp, lle.cpp, boot_main.cpp, game.mm.

Synthetic tests (tests/game/test_module_library.cpp continued):
- A synthetic EBOOT imports the library the PRX exports. Under PS_PPU_MISS_ABORT, sys_prx_load_module and start:
  - place it at its slot with modules_translated() == 1;
  - module_start writes the marker and the call through the repointed import slot returns 5;
  - ps_ppu_dispatch_miss_count() is unchanged and ps_ppu_interp_sites has nothing at or above the floor.
- Oracle (rule 3): the run without the library (interpreter) gives the same marker and return value.
- The module's SPU programs run with no miss.
- A file with one changed byte gets a new identity, goes to main memory, runs interpreted and is captured.
- An edited identity.txt makes the library refuse to load.
- 2,000 load/unload cycles keep prx_dispatch_enabled_ranges() at its starting count + 1.
- A microbenchmark of calls into an enabled module against calls in the main table is recorded, not asserted (runtime.cpp:154-166, 246-251).

GTA V / AC: "modules library: none". gate.sh fps unchanged. The system library's check and run logs are identical to commit 5's run.

COMMIT 10 (D-235) Phone wiring for captured modules

What:
- prepare.mm: ps_prepare_check and ps_prepare_start (prepare.mm:192-228) also run game_modules_are_current and prepare_game_modules after prepare_game, on the same thread with one progress bar.
  - Input is coverage/<TITLE>/prx (coverage derived as at prepare.mm:88-96).
  - The floor comes from the LleModules the system pass already built.
  - A failed modules pass is logged, and the game starts with its modules interpreted.
- game.mm sets RuntimeModules' capture dir to coverage/<TITLE>/prx (beside the SPU dump, game.mm:383-389) and calls GameModules::load.
- scripts/ios.sh pull-coverage also pulls prx/.
- Rule 1: captures live only in Application Support and in inventory/. Check git status anyway.

Files: runtime/platform/ios/src/prepare.mm, runtime/platform/ios/include (prepare.h), game.mm, scripts/ios.sh.

Synthetic test (Mac, rule 2): test_module_library runs two sessions through the same functions prepare.mm calls.
- Session 1 has no library: the module runs interpreted and is captured, and currency is false with the reason "modules to translate".
- After preparing, currency is true.
- Session 2 under ABORT runs it translated.

GTA V / AC: on the Mac, a pspocket-prepare plus pspocket-boot round shows no change. Phone checks come at the end.

COMMIT 11 (D-236) A disc walk as preparation input, so the first session is translated too

What:
- The preparation uses commit 2's 16-byte magic walk over PS3_GAME/USRDIR (ISO or folder), for files up to 256 MB:
  - PPU PRX (e_type 0xFFA4) goes to the modules pass;
  - SPU ELF/SELF (e_machine 23) goes to spu-static over the whole file, which is what sys_spu_image_open and _by_fd hash (runtime/lv2/src/spu.cpp:666-697, 960-995);
  - NPDRM files are counted as "needs licence";
  - other PPU executables are counted.
- The deep data-file and SDATA scan is Mac-only (--deep) until the census shows it pays (fact 12).
- needs_of() for each disc PRX joins choice_for (prepare.mm:286-300; pspocket_prepare.cpp:179-191). Firmware modules that only a game PRX pulls in (lle.cpp:883-890) then get their SPU programs in the system library.
- The walk is cached as game_dir/modules/scan.txt, keyed by image size, mtime and volume id.
- The first time estimate counts PRX MB at the PPU rate.
- ps_prepare_check and ps_prepare_start take the ISO path. Preparer.swift:51-58 and LibraryModel.swift:418-485 pass entry.url and keep its security-scoped access for the whole preparation.
- pspocket-prepare gets --disc ISO|DIR and --list-inputs.
- inputs generation goes to 2.

Files: runtime/disc walk, runtime/prepare/CMakeLists.txt (link disc), prepare.cpp, modules.cpp, prepare.hpp, prepare.mm, prepare.h, Preparer.swift, LibraryModel.swift, pspocket_prepare.cpp, tests/disc, tests/game/test_module_library.cpp.

Synthetic tests:
- An IsoBuilder ISO holds USRDIR/libgame.sprx (debug SELF), USRDIR/spu/task.elf, USRDIR/spu/thread.self, USRDIR/data/level.bin (no magic) and an NPDRM SELF.
- The walk yields 3 inputs and 1 NPDRM, and level.bin is never read past 16 bytes (counted through a wrapper on the memory source).
- The same tree as a folder gives the same result.
- Prepared with empty coverage, a boot runs module_start with no miss, finds the SPU task and the thread-group image with no miss, and leaves coverage/prx and coverage/spu empty.
- A second preparation reads scan.txt, not the disc.

GTA V / AC: record --list-inputs and the walk seconds in the manifest. Modules stay 'none' (verify). PPU and SPU objects are kept. gate.sh passes.

COMMIT 12 (D-237) A stable unit split and per-unit prototypes for the EBOOT lift (prerequisite for commit 13)

What:
- The lifter writes each unit's prototypes into the unit itself. The shared header keeps only register, count and image words.
- Unit split points are kept as addresses in game_dir/game_ppu-units.txt and reused, so a new function joins the unit whose range holds it.
- headers_define hashes only the header a unit includes (prepare.cpp:61-75).
- Without this, each PPU seed recompiles all of GTA V (7:20 on the phone, run 096; fact 5).

Files: recomp/ppu/src/lifter.cpp:1138-1170, recomp/ppu/include/pspocket/recomp/ppu/lifter.hpp, prepare.cpp.

Synthetic test (tests/game/test_game_library.cpp): prepare a synthetic EBOOT, add one seed, prepare again. files_compiled <= 3, and lifting twice is byte-identical.

GTA V / AC: the translator hash changes, which only leaves a note, so there is one full recompile at the next re-preparation for any reason. On the M4, compare prep time and the PPU compile rate before and after (baseline 6:47, D-153), A/B in one session. Ship it in the same release as commit 13, so the phone pays one full recompile.

COMMIT 13 (D-238) Feed PPU misses back into preparation; check currency by content

What:
- ps_ppu_dispatch_miss_stub and ps_ppu_jump_miss (runtime.cpp:246-268) record targets in a lock-free set of 4,096, laid out like D-207's site table (runtime/ppu/src/interpreter.cpp:888-913).
- The host writes coverage/<TITLE>/ppu-misses.txt every 30 s and at exit, through attribute_pc:
  - 'exe <addr>';
  - 'prx <identity> <seg> <off>', relative to the module so it survives the move from main memory to its slot;
  - 'fw <key> <addr>', kept for a later system pass;
  - 'other', counted only.
- prepare_game adds the exe misses to load_executable's seeds (recomp/ppu/src/executable.cpp:11-37). The modules pass adds the prx misses, rebased to the slot. Both carry seeds over in ppu-seeds.txt.
- Currency becomes content-based:
  - a miss not in ppu-seeds.txt;
  - an SPU miss line for a held program that is not in its NAME.seeds (today only newer .bin mtimes count, prepare.cpp:211-218);
  - a captured identity not in identity.txt.
- inputs generation goes to 3.

Files: runtime/ppu/src/runtime.cpp, runtime/ppu/include/pspocket/ppu/runtime.h, game.mm, boot_main.cpp (--coverage DIR), prepare.cpp, prepare.hpp, modules.cpp, prepare.mm.

Synthetic test (two sessions):
- A synthetic EBOOT calls a function through a descriptor built at run time (not in .opd). Session 1 records 'exe' and is not current. After preparing it is current, session 2 passes under ABORT, and files_compiled <= 3.
- A PRX miss recorded while the module sat in main memory seeds the slot copy.

GTA V / AC: compare D-207's top 12 and per-bucket totals before and after one feedback round on the same walk. Expect the exe bucket lower and nothing new interpreted. AC's SPU interpreted count stays at or below 184. gate.sh fps unchanged.

COMMIT 14 (D-239, gated) SPURS job-descriptor scan of the EBOOT's data

What: candidate (eaBinary, sizeBinary) pairs in EBOOT data segments that point into the EBOOT's own PT_LOADs are saved as kind job (relocatable). A false positive only costs compile time, because its hash never matches.

Gate: on the Mac, compare against GTA V's saved job images in inventory (D-117 pulls). It lands only if a meaningful share of the about 256 raw jobs is found statically.

Test: a synthetic EBOOT with a descriptor template pointing at a raw SPU blob. The job runs with no miss.

GTA V: count the first-session dumps before and after.

COMMIT 15 Records

decisions.md entries D-226 to D-239. Update:
- improve.md:195-197 (the L.A. Noire note) and :213-215 (the PRX line);
- status.md; mark D-159's "Not yet" list (decisions.md:2663-2665) as done;
- compatibility.md;
- run notes from 173 on;
- the memory files (run notes, improvement notes, phone findings).

FINAL VERIFICATION (after everything above, by the user, rule 10 and the memory rule "no phone tests until all is implemented")

Ask the user to install with scripts/ios.sh, then:
- GTA V and AC: report the re-preparation time (one full recompile from commit 12) against 7:20 and 3:37 (run 096), then play the usual scenes and report fps against compatibility.md.
- One game never run before (Sniper BLES01281 if its ISO is on the phone): delete pspocket-coverage/<ID> and its library, prepare, play 5 minutes, close, start again, play 5 more. Then run scripts/ios.sh pull-coverage and report:
  - the "engine_jobs runs translated" or "runs under the interpreter" lines;
  - the .bin count in coverage/<ID>/spu and the files in coverage/<ID>/prx after session 1;
  - both sessions' interpreted-PPU buckets.
- Re-check GTA V and every Flawless game in docs/compatibility.md (memory: flawless-bar).

DEFERRED, and why

- Lifter support for link-patched immediates (robust's step 4). The census (commit 2) has to find a game that needs it first (rule 4): GTA V and AC have none (fact 7). If it is built, those instructions also stay out of the analyzer's eval_const and prefix_constants (recomp/ppu/src/analyzer.cpp:131-145, 440-443), with a differential fuzz test (rule 3). Until then the dev guard catches it and such modules are skipped.
- NPDRM klicensee and NP_KLIC_FREE (scripts/self-keys.py:17; recomp/common/src/self_decrypt.cpp:103-106; runtime/hle/src/sceNp.cpp:1539-1553). Disc games rarely need it. This waits until the census shows NPDRM modules on an ISO the user has.
- Deep data-file and SDATA SPU scan on the phone. It reads about 18 GB for GTA V, and D-191 measured about an hour for bulk SPU translation. It stays a Mac --deep census until the numbers show it pays. L.A. Noire's code likely sits in archives (improve.md:195-197), and unpacking those belongs in games/<title>/ (rule 6).
- prx_dispatch_disable. A stable identity, the same slot, the check before linking and the detection of code writes during linking mean an enabled range can only go stale through self-modification, which the dev re-hash after module_start catches. A disable needs safe publication against lock-free readers (runtime.cpp:154-165); it gets built when that guard fires.
- A library ABI layout bump (robust's prx_modules in library.h). It is not needed, and it would recompile every game and the system library (fact: cache_key includes ps_game_abi_hash).
- sys_overlay (syscalls 450-458; runtime/lv2/src/syscall_table.inc:277-285). No game in the inventory uses it. The space between the firmware end and the lowest game slot stays free for it.
- Background or mid-session preparation (plan.md:23, D-147). It needs a BGProcessingTask (device work), and the dispatch tables only register at startup.
- Relocatable PPU lifting. Only if commit 1 shows "other" (heap) code.
- A per-game "modules in main memory" data flag in games/<title>/. Only if a game turns out to depend on its modules sitting in main memory.
- Seeding the system library from 'fw' misses. That changes a library every game shares, so it gets its own decision.
- A flat per-module dispatch array. Only if commit 9's microbenchmark shows calls through the miss stub's linear range scan costing more than a few percent.

RISKS that stay open

- Game modules at 0x38xxxxxx-0x3Fxxxxxx diverge from the PS3 and RPCS3, which use main memory (external/rpcs3/rpcs3/Emu/Cell/PPUModule.cpp:1656). Firmware has lived in that region since D-178 without a report. Sniper is the first real test.
- A firmware layout that grows past a game slot: GameModules refuses to load, the modules run interpreted (logged), and the next preparation lays them out again from the new floor.
- Each SHA-256 check on load runs on a guest thread under RuntimeModules' lock and costs milliseconds per module.
- The first-session SPU gain for GTA V is small: 8 ELFs out of about 265 programs. Most of the gain is in AC (its tasks are EBOOT ELFs) and in games never run before. Commit 14 is the only static path to raw jobs.
