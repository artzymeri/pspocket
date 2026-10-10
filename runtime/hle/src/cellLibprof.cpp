// cellLibprof: the profiler's user trace (libprof: cellUserTrace*), through which a game marks its own events for
// Sony's performance tools, pspocket's own as a console with no profiler attached (D-638). Firmware-first (D-563),
// though no retail system software has the module: libprof belongs to the development tools, so on a retail PS3 these
// imports never resolve (docs/firmware.md), and a game that runs there does not depend on them.
//
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellLibprof.cpp registers the four names only (each a todo answering CELL_OK,
// no arguments), and RPCS3's cellSysmodule.cpp lists the module (external/libprof.sprx, no dependencies). Nothing
// public describes the library. The model, each choice recorded in D-638 with what to check on the Mac:
// - Init and Terminate bracket the library; everything else before Init (or after Terminate), and a second Init,
//   answers CELL_ESTAT. Init's arguments are not read.
// - Register keeps a trace registration under its first argument (what the game names its trace by, as
//   sys_ppu_thread_register_atexit and cellSysutilRegisterCallback keep theirs) with the next two as given, at most 64;
//   Unregister forgets it; Terminate forgets them all. Nothing else is done with them: no profiler reads them here.
// - Error codes: the library's own are not known; these are lv2's (sys/return_code.h, rpcs3/Emu/Cell/ErrorCodes.h),
//   which the system libraries share: CELL_EINVAL for a registration under 0, CELL_EEXIST for one already kept,
//   CELL_EAGAIN past 64, CELL_ESRCH for unregistering one not kept.
// Each process its own library.

#include <map>
#include <mutex>
#include <string_view>
#include <utility>

#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellLibprof";
constexpr const char* kChan = "cellLibprof";

constexpr usize kMaxRegistrations = 64;

constexpr s32 error(lv2::CellError code) { return static_cast<s32>(code); }

struct State {
    std::mutex mutex;
    bool initialized = false;
    std::map<u32, std::pair<u64, u64>> registrations; // the trace's name -> the next two arguments, as given
};

State& state() { return HleProcess::current()->state<State>(); }

// int cellUserTraceInit(...): its arguments are not read (D-638).
s32 cellUserTraceInit(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(lv2::CELL_ESTAT);
    s.initialized = true;
    PS_LOG_INFO(kChan, "cellUserTraceInit: user traces are kept; no profiler reads them here");
    return lv2::CELL_OK;
}

// int cellUserTraceRegister(trace, ...): the registration under `trace`, its next two arguments kept as given.
s32 cellUserTraceRegister(PPUContext&, u32 trace, u64 arg1, u64 arg2)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(lv2::CELL_ESTAT);
    if (!trace) return error(lv2::CELL_EINVAL);
    if (s.registrations.contains(trace)) return error(lv2::CELL_EEXIST);
    if (s.registrations.size() >= kMaxRegistrations) {
        PS_LOG_WARN(kChan, "cellUserTraceRegister({:#x}): {} registrations kept already -> CELL_EAGAIN", trace,
            kMaxRegistrations);
        return error(lv2::CELL_EAGAIN);
    }
    s.registrations.emplace(trace, std::pair{arg1, arg2});
    PS_LOG_DEBUG(kChan, "cellUserTraceRegister({:#x}, {:#x}, {:#x}): {} kept", trace, arg1, arg2,
        s.registrations.size());
    return lv2::CELL_OK;
}

// int cellUserTraceUnregister(trace)
s32 cellUserTraceUnregister(PPUContext&, u32 trace)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(lv2::CELL_ESTAT);
    if (!s.registrations.erase(trace)) return error(lv2::CELL_ESRCH);
    return lv2::CELL_OK;
}

// int cellUserTraceTerminate(void): forgets every registration.
s32 cellUserTraceTerminate(PPUContext&)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(lv2::CELL_ESTAT);
    s.initialized = false;
    s.registrations.clear();
    return lv2::CELL_OK;
}

} // namespace

void register_cellLibprof(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x0252EFCC, cellUserTraceInit),
        PS_HLE_FUNC(0x05893E7C, cellUserTraceRegister),
        PS_HLE_FUNC(0x6D045C2E, cellUserTraceUnregister),
        PS_HLE_FUNC(0x898C77BF, cellUserTraceTerminate),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
