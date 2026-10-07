# Plan: SPU programs translated in the background while the game runs

2026-10-07. Written by a design workflow (four readers: the SPU registry, the compile/sign/load path, where unknown images are met, iOS limits; two designs; two judges; a synthesis), read-only against bb17797. The D-numbers below are the plan's; what landed:

| plan | landed as | |
|---|---|---|
| 1 measure | D-230 | |
| 2 registry | D-231 | |
| 3 negative cache | D-232 | |
| 4 pending home | D-233 | |
| 5 modules, jobs, policy module | D-234 | |
| 6 tasks, lv2 threads | D-233 (test) | no change was needed: the key survives their run loops |
| 7 image hand-off listener | not built | the translator reads the folder the session saves images in (D-236); build it if saving under the lock shows in profiles |
| 8 compile QoS | D-235 | |
| 9 SPU-only libraries | D-236 | |
| 10 SessionTranslator | D-236 | one program per library, the cheapest first; no start-up canary (a failed load stops it) |
| 11 end to end | D-236 (test) | the translator and the switch are tested apart (test_session_translate, test_spu_switch) |
| 12 pspocket-boot | D-236 | |
| 13 app | D-236 | not tried on the phone yet |

Plan: translate SPU code during the session (synthesis; based on per-program, with grafts from session-batch and both judges)

Verified at bb17797 (read-only):
- Jobs attach directly with ps_spu_attach_program_at (spurs_jobchain.cpp:1172), and so does the policy module with ps_spu_attach_program (spurs_custom.cpp:73). Only HLE attach_program goes through identify.
- Object names hash the settings, the include dirs (the source's parent folder), the defines and the text. They do not hash object_dir (build.cpp:155-170, prepare.cpp:89).
- On threads made without a QoS attribute, the SDK's sys/qos.h:83 says they get QOS_CLASS_DEFAULT. That covers build.cpp:287's std::thread and toolchain.cpp:134's llvm::thread, so prepare.mm:250's comment "the compiles it starts inherit it" is wrong.
- set_current overwrites ctx->xfloat (runtime.cpp:119).
- The game.mm lines have moved since D-228: thermal state at :336, memory at :347 and :358, dump folder at :407, load_library at :494.
- The decisions record contradicts itself on 0x04c00: D-204 (decisions.md:4563) says a module, D-205 (:4576) says job chain binaries.

Rules for every commit:
- No edit to spu/runtime.h, spu/context.h or library.h, not even a comment.
- Check that build/host/generated/game/game_abi.c holds the same ps_game_abi_hash before and after.
- New API goes in runtime/spu/include/pspocket/spu/registry.hpp, which is outside PSPOCKET_GAME_ABI_HEADERS. New per-context state goes in ps_spu_host (internal.hpp:13-18) or in thread_local HLE state.
- Tests use synthetic images only (rule 1).
- Each commit records its D-number and updates docs/runs and improve.md (rule 9).

Unless a commit says otherwise, the checks are:
- Assassin's Creed (AC): the run 207 setup, a library prepared from the executable alone (D-226), pspocket-boot for 150 s. Baseline: about 21.6 billion interpreted SPU instructions (19.0 billion at 0x2fba0, 2.6 billion at 0x04c00).
- GTA V: scripts/gate.sh all passes against games/gta5/gate.json, with fps within noise.

COMMITS

1. D-229: measure before building (rule 8)
   What:
   - boot_main prints the total of interpreted SPU instructions (the sum over all 4096 ps_spu_interp_sites, not the top 12), near boot_main.cpp:1340-1347.
   - A docs/runs note sizes the programs at 0x2fba0 and 0x04c00 from AC images saved by earlier runs (inventory/, gitignored): MB of C, estimate_cost, and real compile seconds from pspocket-prepare --events.
   - The note also finds out what 0x04c00 is: a module run (D-204), the job chain's binary page (D-205), or a cut or partial module run with r.hash 0 (spu_programs.cpp:504-518), which is never saved and so could never be translated.
   Sets: the latency target and max_program_seconds.
   Test: none; this is a measurement.
   Check: the AC total matches run 207. gate.sh quick passes.

2. D-230: registry safe for additions while SPU threads run
   Files: runtime/spu/src/runtime.cpp:36-38 and 198-255; new registry.hpp; tests/spu/CMakeLists.txt.
   What:
   - Two counters, registry_additions() and registry_removals(). ps_spu_registry_generation still moves on both.
   - RegistryBatch (RAII, thread_local) bumps the counters once per batch.
   - The table is built outside the lock and published under it only if program->table is still null. If another thread got there first, ours is freed.
   - Unregister moves the table to a list that is never freed and leaves program->table valid. This closes the use-after-free at runtime.cpp:220.
   - find_by_code uses a thread_local scratch buffer instead of allocating a vector per candidate.
   - New helper find_program_checked(hash, ls_start, size, relocatable, ls). It tries find_program, then find_by_code for relocatable images. It requires code_hash != 0 and a code-word hash over local store equal to code_hash.
   - No index by hash; commit 3 makes it unnecessary.
   Test: new tests/spu/test_spu_registry.cpp.
   - (a) Four threads loop find, find_by_code and ps_spu_lookup while 500 programs are registered in batches of 50. Every answer is null or a complete table. Additions move once per batch; removals never move.
   - (b) After unregister, a context still pointing at the program gets the same function (clean under ASan).
   - (c) For an equal key, the first registered still wins (D-155).
   - (d) find_program_checked refuses a flipped code word, accepts a changed data word, and refuses code_hash 0.
   - The existing test_spu_programs.cpp:96-127 still passes.
   Check: AC unchanged; gate unchanged.

3. D-231: negative identification cache (extends D-109)
   Files: spu_programs.cpp:111-163; hle/spu_programs.hpp (stats).
   What:
   - t_known also keeps "no program" entries, with the load-time hash and the additions count.
   - Positive entries check removals only, so a batch landing no longer makes every worker hash every image again.
   - content_check is still computed, because it is the key. Only ps_spu_hash64, the registry scan and the "seen" lookup are skipped.
   - New stat: negative_hits.
   Test: an unknown image loaded three times is hashed once. After its program is registered, the next load finds it. Registering an unrelated program keeps positive entries cached. Unregister still invalidates them.
   Check: AC's negative_hits are large and its interpreted total is unchanged. GTA V fps within noise; SPU busy share the same or lower.

4. D-232: pending home (switch a context that is interpreting right now)
   Files: internal.hpp:13-18; runtime.cpp:115-137, 278-284 and 315-340; channels.cpp:256-292; registry.hpp; spu_programs.cpp:636-646; spurs_jobchain.cpp:1167-1172.
   What:
   - ps_spu_host gets PendingHome {hash, ls_start, size, relocatable, armed, checked}.
   - Every ps_spu_attach_program_at disarms it, including one that attaches null; so does ps_spu_thread_reset. This is what keeps a task's key from leaking into a job or policy-module run on the same worker.
   - HLE attach_program arms it after attaching when identify returns null, storing the additions count read before the lookup.
   - The uncached job path (binary_entry < 0) arms it explicitly after its direct attach.
   - In resolve_program: if there is no home, a host is attached, pending is armed and additions != checked, then:
     - set checked to the current count (so at most one lookup per batch);
     - p = find_program_checked(...);
     - require p->xfloat == ctx->xfloat; otherwise log once and wait for the next load (never overwrite xfloat);
     - ps_spu_attach_program_at, which sets home and home_reloc.
   - stop_at_lifted also counts an armed pending home (runtime.cpp:330), so the switch does not depend on a resolver being installed.
   - Interpreted steps are attributed to the pending hash in a runtime-private table read through registry.hpp. boot_main prints "in unknown image spu_<hash>_<ls>".
   Test: new tests/spu/test_spu_switch.cpp. It uses the real lifted ps_spu_fzp (tests/spu/CMakeLists.txt:20-38), not registered at startup, on a host-attached context on its own thread; the main thread registers the program mid-run.
   - home is set afterwards, and registers and local store equal an interpreter-only run (rule 3);
   - a flipped code word means no switch, and the result is still correct;
   - a different xfloat means no switch;
   - no new registration means zero lookups (counter);
   - contexts without a host behave as before.
   tests/spu/bench_interp.cpp before and after: no change beyond noise.
   Check: AC unchanged (nothing registers yet), but the new lines now attribute steps to images. GTA V fps within noise.

5. D-233: module runs, job cache and policy module pick up new programs
   Files: spu_programs.cpp:197-219 and 475-533; spurs_jobchain.cpp:239-249 and 1165-1197; spurs_custom.cpp:49-77; spurs_native.hpp:113.
   What:
   - Module runs: Run gains an additions count. A run resolved to nothing is looked up again with find_program_checked by r.hash when additions move; the guest copy is never hashed again. Runs with r.hash 0 (cut, or modified and partial) stay interpreted; this is documented.
   - Job cache: CacheEntry gains hash, additions and a sticky "dropped" flag (D-085, D-205). A not-dropped entry with no program is looked up again with find_program_checked(hash, binary, size, true, ls). After the run: if it had a program and home is gone, mark it dropped. Otherwise take ctx.home only if holds(home, binary).
   - Policy module: the worker gains custom_hash, custom_additions and custom_dropped. The module is looked up again at entry, and after ps_spu_run it takes c.home only if home holds pm_entry.
   - New stat: reidentified.
   Tests:
   - test_spu_programs: a module run resolved to nothing runs lifted at its next taken branch after registration, and is not switched when its code words in local store were overwritten.
   - test_spurs_jobchain: a cached binary adopts its program on a later job. A DMA over a code word keeps the entry dropped even across a later registration.
   - test_spurs_run (near :767-775): a resident policy module runs lifted at its next entry.
   Check: GTA V gate; AC unchanged.

6. D-234: tasks that never yield, and lv2 SPU threads
   Files: spurs_taskset.cpp:273-308 and 609-616; lv2/src/spu.cpp:543-569 and 986-996.
   What: make sure a pending home survives re-entries of ps_spu_run in the taskset's syscall loop and in SpuThread::run. The reset at lv2/spu.cpp:987 comes before identify, so the armed key survives.
   Tests:
   - test_spu_threads: a looping lv2 group switches after a program is registered from the PPU side; exit status and memory equal an interpreter-only run.
   - test_spurs_run: a task that never yields switches the same way.

7. D-235: optional hand-off of met images
   Files: spu_programs.cpp:71-107 and 535-562; hle/spu_programs.hpp (MetSpuImage, set_spu_image_listener, flush_spu_images).
   What:
   - With no listener, behaviour is unchanged: the synchronous dump under Registry::mutex, byte-identical, so existing tests need no flush.
   - With a listener:
     - seen.insert stays under the lock;
     - after unlocking, copy the bytes (image.bytes, else base+guest, at most 256 KB) into {hash, kind, guest, ls, size, entry, bytes};
     - push them to a saver thread through a bounded, byte-capped (about 64 MB) queue that never blocks;
     - the saver writes .bin/.txt through a temp file and rename, appends seeds after their image in order, then calls the listener;
     - dump_seed records go through the same queue.
   - On overflow: count the drop and save that image under the mutex, as today. This avoids the race between dump_seed's append and dump()'s truncating .txt write.
   - The saver thread is separate from the translator, so a 75 s compile never delays saving. flush_spu_images() runs at host exit.
   Test:
   - the listener gets the bytes from first sighting even when guest memory is overwritten right after identify;
   - seed lines land after their image's lines;
   - with a tiny cap, items are dropped and counted without blocking, and the image is still saved;
   - without a listener, the files are byte-identical to today's.

8. D-236: explicit QoS for compile threads (codegen)
   Files: build.cpp:287; toolchain.cpp:134; codegen build.hpp and toolchain.hpp.
   What: add an optional qos field to BuildOptions and CompileOptions, applied with pthread_set_qos_class_self_np inside the build worker and inside the llvm::thread lambda (as async_compiler.mm:93 and shader_cache.mm:272 do). When unset, behaviour stays as today. Fix the comment at prepare.mm:250.
   Test: a compile with qos set logs qos_class_self() inside compile_c and the class matches.

9. D-237: building blocks for SPU-only libraries
   Files: runtime/prepare/src/spu.cpp:140-203; prepare.cpp:62-97 and 386-440; new prepare/spu_library.{hpp,cpp} and jobs.{hpp,cpp}; game host.hpp:34-51 and host.cpp:88-140.
   What:
   - lift_spu_image is factored out of lift_spu_images, and its output is byte-identical.
   - job_for and headers_define are shared.
   - ensure_llvm_options: one process-wide call that fails loudly if a later caller asks for different options.
   - compiler_mutex, also held by prepare_game.
   - build_spu_library(program_dirs, options) writes its own info header (PS_GAME_HAS_PPU 0), spu_batch.c and its own objects/ folder, so D-227's cleanup never reaches the game's objects. It uses install name @rpath/pspocket-spu-N.dylib and signs ad hoc or with a key.
   - LoadOptions.spu_only refuses a library with ppu_function_count != 0 or ppu_register set.
   - A failed link is reported, because lld's canRunAgain is ignored (toolchain.cpp:180-184).
   Tests (LLVM-only):
   - test_prepare_spu: lift_spu_image and lift_spu_images give byte-identical files.
   - test_game_library: an SPU-only library from a synthetic image loads; find_program finds the program; running it equals ps_spu_interpret; a PPU library is refused with spu_only; two SPU-only libraries and the PPU test library coexist.
   Check: prepare GTA V again; the result must be "all kept, 0 compiled".

10. D-238: SessionTranslator
    Files: new prepare/session.{hpp,cpp}; runtime/prepare/CMakeLists.txt.
    What:
    - One thread at UTILITY QoS; compiles use qos UTILITY and workers=1. submit and seed never block.
    - Each batch, about 1 s after its first image:
      - skip images already registered (find_program or find_by_code on the copied bytes), same-hash duplicates, and copies with the same code words lifted this session (D-208);
      - lift;
      - order by estimate_cost, cheapest first;
      - send to the next launch (and log) any program whose predicted seconds exceed max_program_seconds (from commit 1). The cap tightens at thermal "fair"; nothing compiles at "serious";
      - start a compile only while available memory minus 1.3 x the predicted peak stays above the floor (1.3 covers the model's 31% error);
      - build, sign and load_library(spu_only) under RegistryBatch, all under compiler_mutex.
    - A signing canary at start (a tiny synthetic SPU-only library, about 50 ms, D-151) turns the translator off right away if signing or loading fails.
    - A failure is logged once and never retried (as D-158). A failed link or load turns the translator off for the session; images are still saved.
    - One log line per program: met at, lift ms, compile s, link, sign and load ms, queue length.
    - stats(): queued, deduped, lifted, compiled, deferred, failed, adopted, and the time from first sighting to registration (median and maximum).
    - stop() cancels and joins.
    - Each batch builds in session_dir/batch-N.
    Test: new tests/game/test_session_translate.cpp (LLVM-only).
    - A task plus three job copies that differ only in data give one library with 2 programs and one additions bump.
    - An image that is already registered is skipped.
    - A low memory callback holds compiles until it rises.
    - An injected failure is logged once, and the next image still lands.
    - A broken toolchain fails the canary and turns the translator off.
    - stop() during a compile returns promptly.

11. D-239: end-to-end synthetic switch (rule 7)
    Test: new tests/game/test_session_switch.cpp (LLVM-only). A real taskset runs a task that GETs a module and loops in it, and an lv2 thread group loops. Both go through the listener and the translator. Within 10 s: the module run resolves to the session program, the thread is adopted, interpreted steps at those LS ranges stop growing, and guest memory equals a run with the translator off (rule 3).

12. D-240: pspocket-boot wiring (Mac first, rule 2)
    Files: runtime/loader/CMakeLists.txt:11-12; boot_main.cpp (merge with D-228's edits).
    What:
    - Only if TARGET pspocket_prepare exists: link prepare and codegen, define PSPOCKET_HAVE_SESSION_TRANSLATOR, and add -no_warn_duplicate_libraries.
    - --spu-translate DIR (under inventory/, gitignored) implies enable_spu_modules, the dump folder and the listener. The toolchain comes from the compile definitions, as pspocket_prepare.cpp:122-131 does.
    - PSPOCKET_SPU_TRANSLATE=0 turns it off for A/B runs. It is off by default, so gate runs are unchanged.
    - Every 10 s, print the interpreted SPU instructions since the last line. At exit, print the translator's stats. The session folder is deleted at start.
    Check:
    - A configure without PSPOCKET_LLVM_DIR in a temporary build/<name>, deleted afterwards, still links pspocket-boot. gate.sh quick passes.
    - AC: runs A, B and C (see below).
    - GTA V: gate.sh game with the translator on and off.

13. D-241: app wiring
    Files: new runtime/platform/ios/src/codegen_env.{hpp,mm}, factored out of prepare.mm:66-87 and 121-163 (signing key and toolchain); game.mm around :494-500; LibraryModel.swift (setting).
    What:
    - Start the translator after the game library loads when PSPOCKET_HAVE_CODEGEN is set, an identity exists and the setting is on. The key is loaded once per session.
    - Memory floor: 1.5 GB (game.mm:358) against 1.3 x the predicted peak.
    - Stop starting compiles for the rest of the session after a memory warning or a DISPATCH_SOURCE_TYPE_MEMORYPRESSURE event.
    - Pause at thermal "serious". Start no new compile after willResignActive, and none in Low Power Mode.
    - Count D-158's 2-4 shader workers in the budget. Refuse to run while the Preparer's g_running is set.
    - On TestFlight, or when the certificate has expired, the canary fails: one visible log line, the translator stays off, and the game keeps interpreting.
    Check: scripts/ios.sh build compiles. The device check comes later and is run by the user (rule 10).

14. Docs (no D-number)
    - plan.md:23 becomes "SPU: translated during the session, refined at the next launch".
    - plan-game-modules.md:381-384 keeps only PPU as deferred.
    - prepare_game removes <game>/session.
    - Rule 5 becomes permanent: log, interpret, translate. Record it in decisions.md.
    - Note that the comment at runtime.h:77 is stale; fix it with the next commit that changes the ABI anyway.
    - Update improve.md (the run 207 entry), compatibility.md and the memory notes.

MEASUREMENT (in one sitting, under caffeinate, no stray processes, build/host only)

AC first session:
- A = --spu-dump only.
- B = --spu-translate, with fresh coverage and session folders.
- C = the full library (22 programs).
- Report: the total interpreted instructions and the 10 s curve, per-program timings, the time from first sighting to the switch for 0x2fba0, deduped and deferred counts, translator CPU seconds and peak RSS, fps, and lifted against interpreted loads.
- Targets: B interprets less than 10% of A, small modules switch within 5 s, and B's last 60 s approach C's rate.
- Then run pspocket-prepare and report kept against compiled.

GTA V:
- gate.sh all with the translator off passes exactly as at bb17797.
- With the translator on: 0 images queued and fps within noise.
- Stress: a library prepared from the executable alone plus --spu-translate. The gate passes; record how many of the 150 programs land, and check that the big ones (35-75 s) are deferred and do not starve the small ones.

Phone (the user runs it, after the Mac passes, on a development install):
- AC for 150 s, setting on, then off.
- GTA V heavy scenes for 5 minutes.
- Report fps, sound pace, thermal state over time, the lowest available memory, and any canary or load failure.
- One TestFlight build, to confirm the translator turns itself off cleanly.

DEFERRED
- Replacing a program with a later lift: RegistryBatch(Supersede) puts the re-lift ahead and bumps removals. It would be safe because tables are never freed. Build it only if measurement shows misses inside session programs cost time.
- Reusing session objects at the next launch. It is possible because object names do not hash object_dir, if the session lifts at <game>/spu/NAME. But later seeds change the text, and the canonical copy within a code-word group must be chosen. Decide after the kept-against-compiled numbers.
- Ranking the queue by interpreted steps. The attribution table is for reporting only until the numbers show a need.
- Index registry lookups by hash.
- Code in no image (cut or partial runs with r.hash 0), unless commit 1 shows it matters.
- PPU translation during a session (ensure_range is startup-only).
- BGProcessingTask and background modes.
- Loading code on TestFlight (D-150, check 1c).
