// The module-management entries of the firmware modules whose libraries pspocket serves itself (D-918, part 2): the
// nameless library every PRX exports (RPCS3's names, PPUFunction.cpp ppu_get_function_name and
// ppu_get_variable_name for the module ""): module_start, module_stop, module_prologue, module_epilogue, module_exit
// and the variable module_info.
//
// A game that loads a firmware module by its path (sys_prx_load_module("/dev_flash/sys/external/....sprx")) whose
// libraries are pspocket's gets no module of Sony's: the loader gives it a module with no code (runtime/loader
// RuntimeModules), the host serving its libraries. These are that module's entries, as a firmware module's are
// reached: liblv2 starts it through module_prologue with module_start (RPCS3 sys_prx_.cpp entryx, D-916 part 1) and
// stops it through module_epilogue with module_stop, and sys_prx_get_module_info reports the two.
// - module_prologue(entry, args, argp): the SDK's (read from librudp.sprx and other modules of fw 4.92) creates the
//   module's lock and runs its C++ static constructors, then calls `entry` with the arguments' low words unless it is
//   all ones, and answers its result (0 without one). The host's libraries have no constructors to run nor a lock to
//   make: their state is the host's. The SDK's also refuses a second start (0x8001000A) by a counter in the module's
//   data; the loader refuses one before (sys_prx_start_module's ALREADY_STARTED), so no counter is kept here.
// - module_epilogue(entry, args, argp): module_stop's result the same way (the SDK's then runs the destructors and the
//   atexit functions and destroys the lock: none here).
// - module_start(args, argp): the host's library is ready: SYS_PRX_RESIDENT (0), as Sony's modules answer.
// - module_stop(args, argp), module_exit(): 0 (some modules export module_exit as their module_stop, libfiber's).
// - module_info: the module's description as a PRX carries it (ppu_prx_module_info: attributes, version, name, TOC,
//   the export and import tables' ranges): one module of the host's, "pspocket", version 1.1, no tables.

#include <cstring>

#include "guest.hpp"
#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {
namespace {

s32 module_start(PPUContext&, u32 /*args*/, u32 /*argp*/) { return 0; }

s32 module_stop(PPUContext&, u32 /*args*/, u32 /*argp*/) { return 0; }

s32 module_exit(PPUContext&) { return 0; }

// The SDK's wrapper: `entry` (all ones: none) with the arguments' low words; its answer's low word.
s32 call_entry(PPUContext& ctx, u64 entry, u64 args, u64 argp)
{
    if (entry == ~u64{0}) return 0;
    return static_cast<s32>(static_cast<u32>(
        call_guest(ctx, static_cast<u32>(entry), {static_cast<u32>(args), static_cast<u32>(argp)})));
}

s32 module_prologue(PPUContext& ctx, u64 entry, u64 args, u64 argp) { return call_entry(ctx, entry, args, argp); }

s32 module_epilogue(PPUContext& ctx, u64 entry, u64 args, u64 argp) { return call_entry(ctx, entry, args, argp); }

// ppu_prx_module_info (0x34 bytes): u16 attributes, u8 version[2], char name[28], u32 toc, exports start and end,
// imports start and end.
void module_info_init(u8* base, u32 addr)
{
    ps_write16(base, addr, 0);
    ps_write8(base, addr + 2, 1);
    ps_write8(base, addr + 3, 1);
    std::memcpy(base + addr + 4, "pspocket", 8);
}

} // namespace

void register_module_management(Registry& r)
{
    // The library has no name (nids.txt's "[]"; the chart calls it "(module management)"); its NIDs are not hashes of
    // the names (RPCS3's fixed ones).
    r.add("", {
        PS_HLE_FUNC(0xBC9A0086, module_start),
        PS_HLE_FUNC(0xAB779874, module_stop),
        PS_HLE_FUNC(0x0D10FD3F, module_prologue),
        PS_HLE_FUNC(0x330F7005, module_epilogue),
        PS_HLE_FUNC(0x3AB9A95E, module_exit),
    });
    r.add_variables("", {
        Variable{{}, "module_info", 0xD7F43016, 0x34, 4, &module_info_init},
    });
}

} // namespace pspocket::hle
