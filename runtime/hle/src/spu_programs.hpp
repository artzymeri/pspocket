#pragma once

// The game's SPU programs as SPURS loads them into a worker's local store (D-076). Each load attaches the lifted
// program for that image when the game's build has one, so the run loop executes native code instead of the
// interpreter. An image is known by the hash of its code as it sits in guest memory, not of the worker's copy (a job
// keeps its data inside its binary, and the copy changes as it runs). Tasks and policy modules are also known by where
// they sit in local store, the address their code was lifted for; job binaries and the modules programs load
// themselves are position independent and land anywhere, so their lifted programs are relocatable (D-081): one
// program per hash, run wherever the image sits.
//
// With a dump directory (pspocket/hle/spu_programs.hpp, pspocket-boot --spu-dump DIR), each image that has no lifted
// program is saved once, for scripts/lift-spu.sh.

#include <functional>

#include "pspocket/core/types.hpp"
#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/spu/context.h"

namespace pspocket::hle::spurs {

enum class ImageKind : u8 { Task, Job, PolicyModule, Module }; // Module: code a program loaded itself (D-079)

struct SpuImage {
    ImageKind kind = ImageKind::Task;
    u32 guest = 0;    // the code's address in guest memory
    u32 ls_start = 0; // where it sits in local store
    u32 size = 0;     // bytes of code (and read-only data) from ls_start, a multiple of 4
    u32 entry = 0;    // where it is entered (LS)
};

// The lifted program for an image just copied into local store (nullptr: the interpreter runs it), from the hash of its
// guest copy at `base + guest`. An image without one is logged (and with a dump directory saved) the first time.
// Hashing costs about what copying the image does several times over: callers identify an image when they load it
// and keep the answer while the image stays in local store (run 050: hashing every entry of GTA V's own policy
// module and every job took a fifth of the workers' time).
const ps_spu_program* identify_program(u8* base, const SpuImage& image);

// identify_program, then attaches the result to the context.
void attach_program(SPUContext& ctx, u8* base, const SpuImage& image);

// The modules a program loaded (D-079) across a task's context switch (D-082). Restoring local store from a save area
// does not go through the MFC, so the runs the tracker knew when the context was saved (those `keep` accepts: inside
// the saved blocks) are kept with the save area and given back when the context is restored, on whichever worker.
// forget_module_runs clears what the tracker knew about the context's local store (another task's image replaced it).
void save_module_runs(const SPUContext& ctx, u32 save_area, const std::function<bool(u32 ls_start, u32 ls_end)>& keep);
void restore_module_runs(SPUContext& ctx, u32 save_area);
void forget_module_runs(SPUContext& ctx);

} // namespace pspocket::hle::spurs
