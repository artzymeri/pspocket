#pragma once

// The game's SPU programs as SPURS loads them into a worker's local store (D-076). Each load attaches the lifted
// program for that image when the game's build has one, so the run loop executes native code instead of the
// interpreter. An image is known by the hash of its code as it sits in guest memory, not of the worker's copy (a job
// keeps its data inside its binary, and the copy changes as it runs), and by where it sits in local store: lifted
// code is specific to its LS address, and position-independent job binaries land on different pages.
//
// With a dump directory (pspocket/hle/spu_programs.hpp, pspocket-boot --spu-dump DIR), each image that has no lifted
// program is saved once, for scripts/lift-spu.sh.

#include "pspocket/core/types.hpp"
#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/spu/context.h"

namespace pspocket::hle::spurs {

enum class ImageKind : u8 { Task, Job, PolicyModule };

struct SpuImage {
    ImageKind kind = ImageKind::Task;
    u32 guest = 0;    // the code's address in guest memory
    u32 ls_start = 0; // where it sits in local store
    u32 size = 0;     // bytes of code (and read-only data) from ls_start, a multiple of 4
    u32 entry = 0;    // where it is entered (LS)
};

// Attaches the lifted program for `image` to the context (or none: the interpreter runs it). The image must be in
// guest memory at `base + guest`.
void attach_program(SPUContext& ctx, u8* base, const SpuImage& image);

} // namespace pspocket::hle::spurs
