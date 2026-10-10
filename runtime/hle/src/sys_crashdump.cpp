// sys_crashdump: the areas of its own memory a game names for a crash dump, pspocket's own (D-607). pspocket makes no
// crash dumps; it keeps each area as it is set and gives it back. Firmware-first (D-563): no Sony module was at hand to
// compare with.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/sys_crashdump.cpp and sys_crashdump.h: sys_crash_dump_log_area_info_t
// (a 16-byte label, the area's address and size: 24 bytes), the indices 0 to 127 (SYS_CRASH_DUMP_MAX_LOG_AREA, which
// RPCS3 notes the SDK does not define) and EINVAL for a higher one or no entry. RPCS3 keeps nothing (its get writes no
// entry); here set keeps the entry's 24 bytes as given and get writes them back, an index never set reading as zeros (no
// label, no area). An entry that is not guest memory is EINVAL here, where RPCS3 checks only for null.

#include <array>
#include <cstring>
#include <mutex>
#include <string_view>

#include "guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "sys_crashdump";

constexpr u32 kMaxLogArea = 127; // the highest index
constexpr u32 kEntrySize = 24;   // label[16], addr, size

using Entry = std::array<u8, kEntrySize>;

struct State {
    std::mutex mutex;
    std::array<Entry, kMaxLogArea + 1> areas{};
};

State& state() { return HleProcess::current()->state<State>(); }

bool usable(PPUContext& ctx, u32 index, u32 entry)
{
    return index <= kMaxLogArea && entry && guest_readable(thread_of(ctx), entry, kEntrySize);
}

// int sys_crash_dump_get_user_log_area(uint8_t index, sys_crash_dump_log_area_info_t *entry)
s32 sys_crash_dump_get_user_log_area(PPUContext& ctx, u8 index, u32 entry)
{
    if (!usable(ctx, index, entry)) return static_cast<s32>(lv2::CELL_EINVAL);
    State& s = state();
    std::lock_guard lock(s.mutex);
    std::memcpy(guest_base(ctx) + entry, s.areas[index].data(), kEntrySize);
    return lv2::CELL_OK;
}

// int sys_crash_dump_set_user_log_area(uint8_t index, sys_crash_dump_log_area_info_t *new_entry)
s32 sys_crash_dump_set_user_log_area(PPUContext& ctx, u8 index, u32 new_entry)
{
    if (!usable(ctx, index, new_entry)) return static_cast<s32>(lv2::CELL_EINVAL);
    State& s = state();
    std::lock_guard lock(s.mutex);
    std::memcpy(s.areas[index].data(), guest_base(ctx) + new_entry, kEntrySize);
    return lv2::CELL_OK;
}

} // namespace

void register_sys_crashdump(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x52696620, sys_crash_dump_get_user_log_area),
        PS_HLE_FUNC(0xB20F87B3, sys_crash_dump_set_user_log_area),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
