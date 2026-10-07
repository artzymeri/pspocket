#pragma once

// The lifted-program registry's host-side API (D-231): what code adding programs while SPU threads run needs (a
// translator working during the session, docs/plan-background-spu.md). Kept out of runtime.h: that header is part of
// a game library's ABI (runtime/game/CMakeLists.txt), and changing it would invalidate every prepared library.
//
// Programs may be registered at any time: a program's lookup table is complete before any thread can find it, and
// the table of an unregistered program stays valid (a context may still run it).

#include <cstdint>
#include <vector>

#include "pspocket/spu/runtime.h"

namespace pspocket::spu {

// Registrations and unregistrations so far. ps_spu_registry_generation moves with both; code that remembered "no
// program" for an image looks again only when additions move, and what it remembered finding holds while removals
// stay the same.
uint64_t registry_additions();
uint64_t registry_removals();

// Registrations on this thread while one lives move the additions count once, when the outermost ends: a library's
// programs land as one batch, and whatever looks again after additions does so once.
class RegistryBatch {
public:
    RegistryBatch();
    ~RegistryBatch();
    RegistryBatch(const RegistryBatch&) = delete;
    RegistryBatch& operator=(const RegistryBatch&) = delete;
};

// The registered program for an image met earlier (`hash` its ps_spu_hash64 then, `size` bytes at LS ls_start) that
// still holds that program's code in `ls` now: found by its key, or by its code words when the image is relocatable
// (D-208), then checked word for word: its code words in local store must hash to the program's code hash. A program
// with no code hash is never returned (nothing to check it against). For switching a context that has run the image
// interpreted since its load, its data changed by then.
const ps_spu_program* find_program_checked(uint64_t hash, uint32_t ls_start, uint32_t size, bool relocatable,
    const uint8_t* ls);

// The context runs an image (hash its ps_spu_hash64 at load, `size` bytes at LS ls_start) that has no lifted program
// (D-233): whenever programs are registered after `additions` (registry_additions() read before the lookup that found
// none), the next taken branch the interpreter runs looks again (find_program_checked) and attaches the program found,
// as home. A context with no host or with a home program keeps none; any attach (ps_spu_attach_program_at) and
// ps_spu_thread_reset forget it. Call on the SPU's thread, after the attach.
void set_pending_home(SPUContext* ctx, uint64_t hash, uint32_t ls_start, uint32_t size, bool relocatable,
    uint64_t additions);
// Contexts switched to a program registered while they ran its image so far.
uint64_t pending_homes_adopted();

// Steps the interpreter ran in images with no lifted program (contexts with a pending home), the n largest.
struct InterpretedImage {
    uint64_t hash;
    uint32_t ls_start;
    uint64_t steps;
};
std::vector<InterpretedImage> interpreted_images(uint32_t n);

} // namespace pspocket::spu
