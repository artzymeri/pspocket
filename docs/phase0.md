# Phase 0: inventory procedure

How to produce the Phase 0 inventory for GTA IV (BLES00229, update 1.08; D-002) and what the exit report must
contain (plan section 10). RPCS3 behaviour cited here was checked against its source (`external/rpcs3` in
`ps3-ios-engine`: upstream 431b169 with the iOS port); file names are given so it can be re-checked after an RPCS3
update.

**Status.** There is no GTA IV dump yet (D-002). Until there is, the toolchain is exercised with synthetic inputs:
`tests/common` builds PPU executables, PRX modules, SPU ELFs and SELF wrappers in code, and the
`ps3inspect_end_to_end` CTest runs the CLI on them. Nothing below needs to happen before a dump exists.

**What Phase 0 needs.** Only the decrypted executables: the update's `EBOOT.BIN` decrypted to `EBOOT.elf`, plus any
PRX/SELF modules the game ships in `USRDIR`. The game assets are not needed until Phase 4/5. The trace step (4)
runs the game in RPCS3, so it needs the full dump installed there.

**Rules.** Work only from a copy the user owns, dumped by the user; never download game images. Never commit
anything this procedure produces (CLAUDE.md rule 1): inventories, traces, extracted SPU programs and logs go to
`inventory/` (gitignored) or stay in RPCS3's own directories. The pre-commit hook (`.githooks/pre-commit`) refuses
PS3 executables, SCE containers and packages.

RPCS3's directories on macOS (`Utilities/File.cpp`, `fs::get_config_dir` / `fs::get_cache_dir`):

| What | Path |
|---|---|
| Config, `dev_hdd0` | `~/Library/Application Support/rpcs3/` (`config.yml`, `dev_hdd0/`, `GuiConfigs/`, `config/custom_configs/`) |
| Log, caches, debug dumps | `~/Library/Caches/rpcs3/` (`RPCS3.log`, `cache/`, `ppu_progs/`, `spu_progs/`, `shaderlog/`) |

The CLI used below is `/Applications/RPCS3.app/Contents/MacOS/rpcs3` (written `rpcs3`).

## 1. Get the decrypted executables

### 1.1 Dump the disc

Dump the user's own GTA IV disc with their own PS3 or a compatible Blu-ray drive, following the "Dumping PS3 games"
part of RPCS3's quickstart guide (https://rpcs3.net/quickstart). The result is a folder with `PS3_GAME/` and
`PS3_DISC.SFB` (or a decrypted ISO). Add it with **File > Add Games** (or boot it once with **File > Boot Game**) and
check that RPCS3's game list shows BLES00229. If the disc is a different region or title ID, stop: D-002 and
`games/gta4/manifest.toml` have to change first.

Running the game (step 4) also needs the PS3 system software installed in RPCS3 (**File > Install Firmware**, with
Sony's official system update file, as the quickstart guide describes). Decryption (1.3) does not: RPCS3's decrypter
has its own key vault.

### 1.2 Install update 1.08

The update package `EP1017-BLES00229_00-GTAIVUPDATEV0108-A0108-V0100-PE.pkg` is already on the Mac (D-002).
Install it with **File > Install Packages/Raps/Edats**, or `rpcs3 --installpkg <file.pkg>` (GUI mode only;
`rpcs3.cpp` refuses installs with `--no-gui`). RPCS3 installs packages to `dev_hdd0/game/<install dir>/`
(`Crypto/unpkg.cpp`, `package_reader::set_install_path`), so the update's executable ends up at

```
~/Library/Application Support/rpcs3/dev_hdd0/game/BLES00229/USRDIR/EBOOT.BIN
```

with its `PARAM.SFO` (APP_VER 01.08) one level up. When a disc game is booted, RPCS3 runs this EBOOT instead of the
disc's if `dev_hdd0/game/BLES00229/PARAM.SFO` has category `GD` (`Emu/System.cpp`, "Updates found at
/dev_hdd0/game/%s/"); the disc EBOOT must still decrypt for that to happen.

### 1.3 Decrypt

Check the licence first: `ps3inspect` reads the plaintext SELF headers without decrypting anything.

```sh
build/host/tools/ps3inspect/ps3inspect "$HOME/Library/Application Support/rpcs3/dev_hdd0/game/BLES00229/USRDIR/EBOOT.BIN"
```

It reports the SELF as encrypted (exit status 3) and prints the app info and, for NPDRM SELFs, the licence type.
What RPCS3 needs (`Crypto/unself.cpp`, `SELFDecrypter::DecryptNPDRM`; `Crypto/key_vault.cpp`):

| `ps3inspect` says | RPCS3 needs |
|---|---|
| no NPDRM header (disc EBOOT, self type APP) | nothing |
| NPDRM licence 3, free | nothing: the key vault starts with the free klicensee |
| NPDRM licence 1 or 2 (network/local) | the content's `.rap` in `dev_hdd0/home/00000001/exdata/` (lowercase extension) |

Disc-game updates are normally free-licence NPDRM SELFs, so no RAP is expected; the check above confirms it. If
decryption still fails, RPCS3 tries the KLIC of the game currently running and then asks for one: boot the game
once (the `sceNp` log line "npDrmIsAvailable(): KLicense key or KLIC=..." prints it), then decrypt again.

Decrypt in the GUI with **Utilities > Decrypt PS3 Binaries** (select the files), or from the command line:

```sh
rpcs3 --decrypt "$HOME/Library/Application Support/rpcs3/dev_hdd0/game/BLES00229/USRDIR/EBOOT.BIN"
```

`--decrypt` runs headless and takes one path per occurrence (repeat it for several files); it prompts on stdin for a
KLIC only when the no-KLIC attempts fail (`rpcs3.cpp`, `Crypto/decrypt_binaries.cpp`). Output goes next to the
input: `EBOOT.BIN` → `EBOOT.elf`, `*.sprx` → `*.prx`, other SELFs → `*.elf`.

Decrypt the same way:

- the disc's `PS3_GAME/USRDIR/EBOOT.BIN` (to confirm the disc version; its code is not recompiled),
- every `*.sprx` / `*.self` / `*.prx` under the disc's and the update's `USRDIR`
  (`find <dir> -iname '*.sprx' -o -iname '*.self' -o -iname '*.prx'`).

Second route, which also catches modules the game loads at runtime: enable **PPU Debug** (step 4.1) and boot the
game. RPCS3 then writes every decrypted executable and every PRX loaded through `sys_prx_load_module` to
`~/Library/Caches/rpcs3/ppu_progs/BLES00229-<hash>-<file>/exec.elf` or `prog.prx` (`Emu/System.cpp`,
`dump_executable`; `lv2/sys_prx.cpp`). Firmware modules show up there too: they are Sony's, inventory them but
never copy them anywhere else.

## 2. Run ps3inspect

Build the toolchain (`cmake -S . -B build/host -G Ninja -DPSPOCKET_WERROR=ON && ninja -C build/host`), then keep
every output under `inventory/` (gitignored):

```sh
I=inventory/gta4; mkdir -p $I
U="$HOME/Library/Application Support/rpcs3/dev_hdd0/game/BLES00229/USRDIR"
P=build/host/tools/ps3inspect/ps3inspect

$P --json $I/eboot.json "$U/EBOOT.elf" > $I/eboot.txt               # the main inventory
$P --quiet --json $I/selfs.json "$U/EBOOT.BIN" <disc>/PS3_GAME/USRDIR/EBOOT.BIN   # SELF headers, licence
$P --json $I/prx.json <decrypted .prx files> > $I/prx.txt
$P --quiet --strings --min-string 8 --json $I/strings.json "$U/EBOOT.elf"         # middleware review
$P --quiet --spu-extract $I/spu "$U/EBOOT.elf"                     # SPU images for Phase 3 (stays in inventory/)
```

Options: `--names <file>` adds NID names (`[module]` headers, `name` or `0xNNNNNNNN name` lines). Exit status: 0 ok,
1 usage or I/O error, 2 malformed or unknown input, 3 encrypted SELF.

What to check in the output:

- **Type** `PPU executable`, machine PPC64, and the `RPCS3 hash` line (`PPU-<sha1>`): it must equal the "PPU executable
  hash" RPCS3 logs when booting the update. Likewise each SPU image's `SPU-<sha1>` must equal RPCS3's "SPU executable
  hash" lines. A mismatch means the two tools are not looking at the same binary.
- **Process param**: SDK version, primary thread priority and stack, malloc page size.
- **Imports**: every library with its NIDs. `?` is a NID the database can't name; "named via another module" means the
  name comes from a different library's registration (check it). The NID database is RPCS3's registrations
  (`recomp/common/data/nids.txt`, regenerated by `recomp/common/scripts/harvest_nids.py <rpcs3-dir>`).
- **Function descriptors**: the count and how they were found (`.opd` section, section probe, or entry-segment scan
  when the decrypted ELF has no usable section headers).
- **SPU images** and **middleware signatures**.
- `sha256` of each file: copy it into `games/gta4/manifest.toml` (`[[files]]`) so builds can verify their input.

`ps3inspect` sees SPU programs stored as ELF images. Code the game builds or DMAs at runtime (SPURS jobs, overlays)
only shows up in the trace (step 4).

## 3. Phase 0 exit report

The exit criterion (plan section 10): an inventory report with the function count, imported NIDs per module and SPU
programs split into Sony middleware versus custom, then a go/no-go decision and a re-estimate. The report is derived
from the game, so it lives in `inventory/gta4/report.md`, not in git; `docs/decisions.md` gets the decisions and the
re-estimate, without the tables. The report contains:

1. **Inputs.** Title ID, region and APP_VER as found; SHA-256 of every input file (also in the manifest); RPCS3 version
   used; the `PPU-` hash.
2. **Executable.** Segments, SDK version, process parameters, TLS size, PRX modules shipped or loaded at runtime.
3. **Function count.** The descriptor count from `ps3inspect`, and the code size (executable segments). A refined count
   comes from the Phase 1 analyzer.
4. **Imports per module.** Function and variable NIDs per library, each with its name; the unresolved list (each
   needs a name before Phase 2 can stub it); calls the trace saw (step 4) versus imports never called.
5. **Syscalls** used, with counts, from the trace.
6. **SPU programs.** Embedded images (count, total size, SHA-1, name, class) plus programs seen only at runtime;
   split into Sony middleware, third-party middleware and custom code, with sizes; SPURS usage (workloads, tasksets,
   job chains).
7. **Middleware** found (RAGE, Euphoria, others) and what it implies for PPU or SPU work.
8. **Rendering.** Render-target readbacks (count, call sites, frequency), number of distinct vertex and fragment
   programs.
9. **Decisions it settles** (plan section 14): SPURS HLE or LLE; the module list for Phase 2 (only what the game
   uses, CLAUDE.md rule 4); SPU replacement candidates (plan section 6); D-002 confirmed or changed.
10. **Go/no-go and re-estimate** per phase, with the reasons.

## 4. Trace a 30-minute session in RPCS3 (checklist)

Plan section 10: a 30-minute desktop session including driving, logging SPU programs loaded (with hashes),
syscalls used, render-target readbacks and the shader count. Items marked **stock** work with an unmodified RPCS3;
**patch** needs a local RPCS3 build with extra logging (in `ps3-ios-engine/external/rpcs3`, never in this repo).

### 4.1 Setup

- [ ] Per-game config (`config/custom_configs/config_BLES00229.yml`, from the game's context menu), with the
      settings that make GTA IV run correctly in RPCS3 (see the ps3-ios-engine notes).
- [ ] Debug options. The Debug tab is hidden by default: set `showDebugTab=true` in the `[Meta]` section of
      `GuiConfigs/CurrentSettings.ini`, or edit the YAML keys directly. Enable `Core: PPU Debug` (dumps decrypted
      executables and PRXs), `Core: SPU Debug` (dumps embedded SPU images to `spu_progs/`) and
      `Video: Log shader programs` (writes every translated shader to `shaderlog/`).
- [ ] Leave `Write Color Buffers` and `Read Color Buffers` off (the defaults), so readbacks show up as faults.
- [ ] Empty `~/Library/Caches/rpcs3/spu_progs/` and `shaderlog/` before the session; note the RPCS3 version.

### 4.2 Session

- [ ] Boot, menus, load a save or start a new game, walk, drive for a sustained stretch, a cutscene, the phone, the
      map and pause menu, a mission start. Note timestamps of each part so log lines can be attributed.
- [ ] Right after quitting, copy `RPCS3.log` (RPCS3 replaces it on the next launch), `spu_progs/`, `shaderlog/`,
      `ppu_progs/` and the game's cache directory (step 4.3) to `inventory/gta4/trace-<date>/`.

### 4.3 What to log

- [ ] **Syscalls (stock).** `lv2/lv2.cpp` (`ppu_syscall_usage`) logs "PPU Syscall Usage Stats" with each syscall's
      name and count every ~10 s and at exit; take the last block.
- [ ] **Embedded SPU images (stock).** "SPU executable hash: SPU-<sha1>" lines with segment dumps and SPUNAME, from
      `PPUModule.cpp` (`ppu_check_patch_spu_images`), plus the `spu_progs/` dumps. Match against `ps3inspect`.
- [ ] **SPU code executed (stock).** The SPU cache `~/Library/Caches/rpcs3/cache/BLES00229/ppu-<hash>-EBOOT.BIN/
      spu-<block size>-v1-tane.dat` holds every SPU function RPCS3 compiled (entry, bounds, code); the log line
      "Cache: ..." names the directory. Keep a copy: a later pspocket tool hashes these programs.
- [ ] **SPU programs loaded (patch).** In `lv2/sys_spu.cpp`: `sys_spu_image_open`, `_sys_spu_image_import`,
      `sys_spu_thread_initialize`, `sys_spu_thread_group_create`, `sys_spu_thread_group_start`,
      `sys_raw_spu_create` (and raw SPU local-store writes). Log the image source (path or address), the LS address
      and size of each segment and a SHA-1 of the loaded bytes, the group name and size.
- [ ] **SPURS (patch).** In `Modules/cellSpurs.cpp`: `cellSpursAddWorkload` / `cellSpursAddWorkloadWithAttribute`
      (policy module address, size, SHA-1), `cellSpursCreateTaskset*`, `cellSpursCreateTask` (task ELF address and
      SHA-1), `cellSpursCreateJobChain*` / `cellSpursRunJobChain` (job binaries: address, size, SHA-1). If the
      session runs SPURS as LLE firmware code instead, log the same at the `sys_spu` level and note it.
- [ ] **Render-target readbacks (patch).** `RSX/VK/VKGSRender.cpp`, `VKGSRender::on_access_violation` (RPCS3 renders
      through Vulkan/MoltenVK on macOS): when `!is_writing && result.num_flushable > 0`, log the fault address, the
      flushed sections (range, format, size), the frame number and the guest PC of the access (PPU thread `cia`, or
      the SPU and its DMA). Aggregate by PC and surface: these are the patch candidates for the manifest.
- [ ] **Shaders (stock).** Count the distinct `VertexProgram*` and `FragmentProgram*` files in `shaderlog/`.
- [ ] **Firmware calls (stock).** "TODO" and error lines from module channels (unimplemented or failing HLE calls),
      and the PRX modules loaded (`sys_prx` lines, `ppu_progs/`).
- [ ] **Optional (patch)**: per-function HLE call counts (counter in the HLE call path, printed at exit), files opened
      through `sys_fs_open` (streaming pattern), PPU threads created (name, priority, stack), memory high-water mark
      (`sys_memory_*`, `sys_mmapper_*`).
