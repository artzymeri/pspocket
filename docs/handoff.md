# Handoff: pick up where the last session stopped (2026-10-07, ~12:00)

You are continuing a long-running engineering effort on **pspocket** (this repository,
github.com/artzymeri/pspocket, branch `main`). The previous Claude session ran out of room; this file is everything it
knew that is not already in the docs. Read it fully, then `docs/status.md`, `CLAUDE.md`, the latest entries of
`docs/decisions.md` (D-203 to D-219 are the most recent) and `docs/runs/README.md`.

## 1. What we are building, and the goal

pspocket is a **static recompiler that runs PS3 games natively on the iPhone**. A game's PPU (PowerPC) and SPU code is
translated ahead of time into C, compiled to ARM64, signed and loaded; its graphics go to Metal, its sound to
AVAudioEngine. It is **not an emulator**: the interpreters exist only as the reference ("oracle") for testing and as a
safety net whose misses are recorded and translated next time.

**The user's goal, in their words:** "a portable PS3 that runs native on iOS"; "if I have the PS3 game ISO, I need to
be able to run it flawlessly"; "PS3 iOS running natively, an engine for any game, even unknown ones, to run flawlessly
on our app". Non-negotiable (2026-10-06): "our own PS3 game reader for ANY PS3 game... iOS native infrastructure, no
emulation, no interpreting". So:

- **Route B (decided 2026-10-05, D-147):** the phone alone takes any ISO, translates the game once on the phone
  ("Preparing game…": lift PPU+SPU to C, compile with LLVM built into the app, sign, dlopen). No Mac per game.
- Every system library should be pspocket's own on iOS frameworks; the user's PS3 firmware file (PS3UPDAT.PUP) is only
  a stopgap, and its modules are translated once (D-178), never interpreted.
- "Flawless" is a per-game bar in `docs/compatibility.md` (own frame rate for a full session, graphics match the PS3,
  sound in time, saves work, no crash in an hour). Games are Flawless / Playable / Boots / Broken. Fixes are general
  (no title checks in the runtime).
- Driving title: **GTA V (BLES01807)**. Second: **Assassin's Creed (BLES00158)**. L.A. Noire (BLES00933) is on the
  phone only (Broken, untested since D-165).

## 2. How to work with the user (standing rules, from them)

- "I'm not a programmer. Don't ask me design questions: decide, record each decision in docs/decisions.md, and explain
  things to me in plain words."
- "Work on main only: commit to main and run `git push origin main` after each piece, so nothing is lost if the
  session stops. This is my explicit permission to push to main. Keep commits small, one per component (rule 9). No
  pull requests unless I ask."
- Agents (2026-10-10, replacing "work with 3 agents now"): "Run as many agents as you can": as many helper agents
  as the machine allows (a cloud container has 4 cores, 15 GB and about 25 GB of free disk: six at a time, each in
  its own git worktree and build directory, the builds sharing ccache: configure with
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache and ccache's base_dir set to the
  repository); the main session reviews each one's commits, merges them into main and pushes. Another session on
  the Mac (with the firmware) works on main too and numbers its decisions from D-900.
- Follow `CLAUDE.md`: never commit game data (ISOs, ELFs, firmware, keys, lifted code, logs: all stay in the
  gitignored `inventory/`); tests use synthetic data built in code; cite RPCS3 in a comment wherever behaviour comes
  from it; unimplemented calls log and return an error, never succeed silently; no title checks in the runtime.
- `scripts/ios.sh testflight` uploads the app (with the game's translated code) to Apple: **only with the user's OK,
  each time.**
- **Don't ask the user to test on the iPhone until everything is implemented** ("dont tell me to test on iphone until
  you implemented everything").
- Firmware, decrypted Sony modules and disassembly stay in scratch or `inventory/`, never in the repo (D-046).
- Every outside dependency lives inside this folder under `external/` (gitignored: `external/rpcs3`,
  `external/llvm-project`), never beside the repo ("next time keep it inside pspocket bro").
- Keep one Mac build dir, `build/host` (configured with the game's lifted code, below); delete old builds and frame
  dumps; the disk is ~87% full (~55 GB free).
- After every run and fix, write it down at once: a run note in `docs/runs/` + its row in `docs/runs/README.md`, a
  `docs/decisions.md` entry (next free number: **D-220**), and update `docs/improve.md`, `docs/status.md`,
  `docs/compatibility.md`. Run the gate (`scripts/gate.sh all`) after each change.
- The user's last instruction before this handoff: "continue your work, whatever you need, don't stop until we have
  achieved the PS3 iOS native engine for any game, even unknown ones, to run flawlessly on our app".

## 3. Where things stand (short)

- **GTA V**: Playable. On the Mac it passes the gate (prologue walk, ~29.8 fps = the game's 30 cap). On the iPhone
  (last tried 2026-10-05/06, builds up to D-164): 30 fps in light scenes, 10 to 20 in heavy ones (CPU-bound: SPURS /
  EDGE geometry jobs), sound slows to 0.5-0.8 pace when cores are full. Everything since D-165 is Mac-only; the phone
  will re-prepare every game on its next start.
- **Assassin's Creed**: on the Mac, plays into the Animus training (Altaïr walks, pause menu, the "Gentle Push" crowd),
  57.7 fps median in the lab with the GPU, ~33 fps in the crowd; SPU code interpreted: 184 instructions a session;
  no unimplemented calls. Its library is prepared on the Mac the way the phone does it (`pspocket-prepare`).
- This week's work was mostly **speed** (the phone is CPU-bound): RSX per-draw cost 13.0 to 9.2 µs (D-210, D-211,
  D-214), fiber switches without C++ exceptions (D-213), and the SPU translator (D-215 to D-219): GTA V's main SPU
  threads went from 7.93 to about 6.56 s of CPU per 5 s (-17%), the audio SPU -25%.

## 4. What the last session did (2026-10-07), all committed and pushed except section 5

| commit | what |
|---|---|
| 7b8e2b2 | **D-218** shufb's special control bytes in one table lookup (`runtime/spu/include/pspocket/spu/lifted.h`). Within noise (6.95 to 6.91 s/5 s). |
| 1c65748 | **D-219** SPU functions load registers where needed and store only what changed (`recomp/spu/src/lifter.cpp` RegFlow/Exits/FunctionFlow; `recomp/spu/src/analyzer.cpp` + `analyzer.hpp` gained `Function::dynamic`; tests `tests/spu/test_lift_regs.cpp`, program-level fuzz extended in `tests/spu/fuzz_gen.cpp` / `test_fuzz.cpp`). GTA V main SPU 7.00 to 6.56 s/5 s (-6%), audio SPU 1.57 to 1.39 (-11%). spu-check 150 programs / 5,002 trials, 0 differ; gate PASS. |
| d7d5ceb | Docs: runs 140-141 (in `docs/runs/134-gta5-spu-profile.md`), improve.md. |

After the commits: Assassin's Creed was re-prepared with D-219 (`inventory/BLES00158/prepared/game.dylib`, 2:38) and
`spu-check --library` on it passed (22 programs, 1,446 trials, 0 differ). Its measurement run (run 142, the training
walk, 600 s, `inventory/BLES00158/run-142.log`) was first spoiled by the Mac going to sleep (lid closed, ~200 s freeze);
it was started again under `caffeinate` and may have finished when you read this. Compare it with run 139 (section 7,
`acmeasure.py`); if it is incomplete (fewer than ~115 "fps (" lines), run it again. Then record runs 142 in a run note
and in compatibility.md (AC row: D-219 numbers).

## 5. In flight, NOT committed: D-220, the SPU module tracker (`runtime/hle/src/spu_programs.cpp`)

**Why:** the "module tracker" watches every SPU DMA GET into local store so that code a program copies in itself
(modules) is recognised and runs translated (D-079). Its GET observer costs ~240 ns per GET, 11-12% of busy SPU time
in Assassin's Creed's crowd (1.6-3 million GETs/s) and ~3.4% in GTA V (runs 126-127, improve.md "Memory" section).

**The change (written, compiles untested — `git diff runtime/hle/src/spu_programs.cpp tests/hle/test_spu_programs.cpp`):**
the per-1 KB "Block" lists of runs are replaced by an **owner table**: each 16-byte line of local store holds the id
of the run (contiguous copy) that wrote it last (`Modules::owner`, 16,384 u32 per SPU thread); runs live in a ring of
4,096 indexed by `id % kRuns` and are valid only while `runs[id % kRuns].id == id` (ids increase; `first_id` forgets
everything in O(1); ids wrap after 4 billion GETs with a full clear). A GET: marks the runs it writes over as needing
identification again, then extends the last run (contiguous in LS and guest) or starts a new one, and fills its lines.
`resolve(pc)`: the run is `owner[pc >> 4]`; its part still in local store is the stretch of lines around pc it still
owns (computed when it is identified again; a cached identification is reused only while pc is inside that stretch);
then the old logic (memcmp with the guest copy, D-206 local-store identity, `lifted_outside` D-085). `reset`,
`save_module_runs` (scans the owner table for stretches) and `restore_module_runs` are ported. A new test
"a module with data copied into its middle runs lifted in the part holding its code (D-220)" was added to
`tests/hle/test_spu_programs.cpp` (new behaviour: before, only the part before the data was kept).

**To finish it:**
1. Review the diff, then `ninja -C build/host` and `ctest --test-dir build/host --output-on-failure` (especially
   `build/host/tests/hle/test_hle "*SPU programs*"`).
2. Measure, same session, before and after (rule 8): Assassin's Creed's training walk (section 7) with the committed
   tracker vs the new one; the crowd's SPU CPU per 5 s (last 12 windows) and fps; `sample` profile share of
   `observe_get`. Also GTA V (section 7) and `scripts/gate.sh all`. Also check "SPU program loads: N lifted, 0
   interpreted" and "SPU interpreted: 184 instructions" do not get worse in AC's log.
3. If it helps and nothing regresses: decision **D-220** in `docs/decisions.md`, a run note, improve.md (Memory
   section: the GET observer item), commit, push. If it does not help: `git checkout` the two files and record the
   attempt in improve.md (rule 8).

## 6. What to do after that (from `docs/improve.md`, pick by evidence)

- SPU: stores at function exits barely fell with D-219 (46.4 to 42.9 per exit): a caller could tell the callee which
  written registers it reads after the call. `scripts/lift-spu.sh` carries all of the previous lift's labels over as
  seeds (prepare keeps only used seeds since D-205): make it keep `NAME.seeds` like prepare. Quadword loads/stores
  (byte reversal + lane extraction for addresses) are ~13% of lifted SPU samples; shufb ~10%; C-compiler spills
  ("line 0") ~10%.
- RSX thread (AC's crowd, ~2,000 draws/frame): read the FIFO in runs of words per IO page (~15% of the thread),
  upload transform constants only from the lowest written to the highest used register, multisampled targets (drawn
  with one sample today), fewer render passes.
- Audio pace on the phone when SPUs are saturated: give the workload feeding cellAudio priority.
- Assassin's Creed: further into the game (walks in `games/ac1/`); LS 0x0e408 interpretation item (improve.md).
- Then the phone: a build with everything since D-164 must be installed and both games re-prepared, **but only when
  the user agrees the set is complete** (see section 2). TestFlight only with the user's OK.

## 7. Commands that work (Mac, M4)

```sh
# build + tests (build/host is configured with the game's lifted code:
#   -DPSPOCKET_GAME_PPU=inventory/gta5/ppu-lifted -DPSPOCKET_GAME_SPU=inventory/gta5/spu-lifted -DPSPOCKET_WERROR=ON)
ninja -C build/host && ctest --test-dir build/host --output-on-failure
scripts/gate.sh all            # tests + GTA V prologue walk vs games/gta5/gate.json; a 3rd frame jump ~frame 3246 is a known flake: rerun

# GTA V walk, 128 s (measure the 60-90 s windows)
build/host/runtime/loader/pspocket-boot inventory/gta5/decrypt/EBOOT.elf --gpu metal --timeout 128 \
  --hdd0 inventory/gta5/hdd0 --bdvd inventory/gta5/disc.iso --flash inventory/firmware/dev_flash \
  --shader-cache inventory/gta5/shaders --press-file games/gta5/walk-prologue.txt > LOG 2>&1

# GTA V SPU re-lift after a lifter change (then rebuild; keep a backup of the old dir for A/B)
scripts/lift-spu.sh inventory/gta5/spu inventory/gta5/spu-lifted
build/host/runtime/loader/spu-check inventory/gta5/spu --trials 2 --entries 40     # must say "0 differ"

# Assassin's Creed: prepare as the phone does, check, play the training walk (600 s; the crowd is at the end)
build/host/runtime/prepare/pspocket-prepare --title BLES00158 --exe inventory/BLES00158/decrypt/EBOOT.elf \
  --game-dir inventory/BLES00158/prepared --spu-images inventory/BLES00158/spu-images --spu-log inventory/BLES00158/run-116.log
build/host/runtime/loader/spu-check inventory/BLES00158/spu-images --library inventory/BLES00158/prepared/game.dylib --trials 2 --entries 40
caffeinate -i -s build/host/runtime/loader/pspocket-boot inventory/BLES00158/decrypt/EBOOT.elf --bdvd inventory/BLES00158/disc.iso \
  --hdd0 inventory/BLES00158/hdd0 --flash inventory/firmware/installed/dev_flash \
  --game-library inventory/BLES00158/prepared/game.dylib --timeout 600 --press-file games/ac1/walk-training.txt \
  --rsx-recover --gpu metal > inventory/BLES00158/run-NNN.log 2>&1

# profile a running pspocket-boot (GTA V at ~65-95 s, AC's crowd at ~550-600 s)
sample <pid> 15 -file sample.txt
# SPU differential fuzz, longer: PSPOCKET_FUZZ_ITERS=1000 PSPOCKET_FUZZ_SEED=7 build/host/tests/spu/test_spu_fuzz
```

Note: pspocket-boot without `--gpu metal`/`--frames` uses a null GPU (fps then says only how fast the CPU side is).

Measurement helpers (put them in your scratch directory):

```python
# measure.py LOG...  -> GTA V: mean of the 60-90 s windows
import re, sys, statistics as st
for path in sys.argv[1:]:
    t=0; rows=[]; fps=0
    for line in open(path, errors='replace'):
        m=re.search(r'([\d.]+) fps \(', line)
        if m: t+=5; fps=float(m.group(1))
        m=re.search(r'threads: CPU ([\d.]+) s in', line)
        if m and 60<=t<=90:
            spu=sum(float(x) for x in re.findall(r'SPURS 10015000 SPU \d ([\d.]+) s', line))
            sec=sum(float(x) for x in re.findall(r'SPURS 10016000 SPU \d ([\d.]+) s', line))
            rows.append((fps,float(m.group(1)),spu,sec))
    print(path, 'fps %.1f cpu %.2f spu5 %.2f spu-secondary(audio) %.2f'%tuple(st.mean(x[i] for x in rows) for i in range(4)))
```

```python
# acmeasure.py LOG...  -> Assassin's Creed training walk: the last 12 windows (the crowd)
import re, sys, statistics as st
for path in sys.argv[1:]:
    rows=[]; fps=[]
    for line in open(path, errors='replace'):
        m=re.search(r'([\d.]+) fps \(', line)
        if m: fps.append(float(m.group(1)))
        m=re.search(r'threads: CPU ([\d.]+) s in', line)
        if m:
            spu=sum(float(x) for x in re.findall(r'SPURS 102ec580 SPU \d ([\d.]+) s', line))
            rsx=sum(float(x) for x in re.findall(r'pspocket.rsx ([\d.]+) s', line))
            rows.append((float(m.group(1)),spu,rsx))
    last=rows[-12:]
    print(path, len(rows), 'windows; last 12: cpu %.2f spu5 %.2f rsx %.2f'%tuple(st.mean(x[i] for x in last) for i in range(3)),
          'fps last 12 %.1f, median %.1f'%(st.mean(fps[-12:]), st.median(fps)))
```

Reference numbers: run 139 (AC with D-215..D-217): crowd SPU (5 threads) ~13.9 s per 5 s, process ~20.9, ~33 fps.
GTA V (same session, 2026-10-07 ~10:00): before D-219 6.98-7.01 main SPU / 1.56 audio; after 6.53-6.62 / 1.38-1.39.

## 8. Measurement pitfalls (they cost hours on 2026-10-07)

- Check for leftover processes before measuring (`top -l 2 -o cpu -n 8`): a stray script made every run look 16%
  slower. An iOS Simulator running PSPocket has been up for days (~0.4 core): constant, leave it.
- Long runs: wrap in `caffeinate -i -s` (the Mac slept mid-run when the lid closed).
- A/B by swapping lifted directories: `mv` keeps old file times, so ninja rebuilds nothing. After a swap run
  `find DIR -type f -exec touch {} +`. Do before/after in one session, two runs each side.

## 9. Phone facts (for later, not now)

iPhone 17 Pro Max (A19 Pro: 2 performance + 4 efficiency cores), iOS 26. By cable: `scripts/ios.sh build|install|run`;
logs: `scripts/ios.sh session`, `idevicesyslog -p PSPocket`. Away from the Mac: TestFlight (app "PSPocketTEST"),
`scripts/ios.sh testflight`, only with the user's OK each time; credentials are already set up on this Mac (never
print key files). Open phone items are in `docs/compatibility.md` and `docs/improve.md` (memory spike fix D-148 and
background shader compiles D-158 not yet confirmed on the phone).
