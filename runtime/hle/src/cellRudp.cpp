// cellRudp: reliable UDP (librudp), its life: Init, End and the event handler, pspocket's own (D-607). The rest of the
// library (contexts, binding, reading and writing, polling: 28 functions) needs Sony's module, read or compared on the
// Mac; they stay unimplemented (logged, an error), and the library is firmware-first (D-563): where the system software
// is installed a game gets Sony's librudp whole.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellRudp.cpp and cellRudp.h: the error codes, CellRudpAllocator (app_malloc,
// app_free), Init (a second Init ALREADY_INITIALIZED; without an allocator the library's own), End and
// SetEventHandler (NOT_INITIALIZED before Init; End forgets the allocator and the handler). An allocator that is not
// guest memory is INVALID_ARGUMENT here, where RPCS3 reads it unchecked. Nothing calls the allocator or the handler yet:
// they are kept for the library's other functions.

#include <mutex>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellRudp";
constexpr const char* kChan = "cellRudp";

enum : u32 {
    CELL_RUDP_ERROR_NOT_INITIALIZED = 0x80770001,
    CELL_RUDP_ERROR_ALREADY_INITIALIZED = 0x80770002,
    CELL_RUDP_ERROR_INVALID_ARGUMENT = 0x80770004,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// CellRudpAllocator: the descriptors of app_malloc(size) and app_free(ptr).
constexpr u32 kAllocatorSize = 8, kAppMalloc = 0, kAppFree = 4;

struct State {
    std::mutex mutex;
    bool initialized = false;
    u32 app_malloc = 0, app_free = 0; // 0: the library's own allocator
    u32 handler = 0, handler_arg = 0; // CellRudpEventHandler(event_id, soc, data, datalen, addr, addrlen, arg)
};

State& state() { return HleProcess::current()->state<State>(); }

// int cellRudpInit(CellRudpAllocator *allocator): NULL for the library's own.
s32 cellRudpInit(PPUContext& ctx, u32 allocator)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(CELL_RUDP_ERROR_ALREADY_INITIALIZED);
    if (allocator && !guest_readable(thread_of(ctx), allocator, kAllocatorSize))
        return error(CELL_RUDP_ERROR_INVALID_ARGUMENT);
    s.initialized = true;
    s.app_malloc = allocator ? ps_read32(guest_base(ctx), allocator + kAppMalloc) : 0;
    s.app_free = allocator ? ps_read32(guest_base(ctx), allocator + kAppFree) : 0;
    PS_LOG_INFO(kChan, "cellRudpInit: {}", allocator ? "the game's allocator" : "the library's own allocator");
    return lv2::CELL_OK;
}

// int cellRudpEnd(void)
s32 cellRudpEnd(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    s.initialized = false;
    s.app_malloc = s.app_free = 0;
    s.handler = s.handler_arg = 0;
    return lv2::CELL_OK;
}

// int cellRudpSetEventHandler(CellRudpEventHandler handler, void *arg): NULL takes the handler away.
s32 cellRudpSetEventHandler(PPUContext&, u32 handler, u32 arg)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RUDP_ERROR_NOT_INITIALIZED);
    s.handler = handler;
    s.handler_arg = arg;
    return lv2::CELL_OK;
}

} // namespace

void register_cellRudp(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x63F63545, cellRudpInit),
        PS_HLE_FUNC(0xB6BCB4A1, cellRudpEnd),
        PS_HLE_FUNC(0x7ED95E60, cellRudpSetEventHandler),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
