// The game process's system services: sys_game (the watchdog, the system software version, the arcade board storage,
// the RTC status) and sys_ss (random numbers, the console's PSID and IDs, the secure RTC, access control).
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_game.cpp, sys_ss.cpp.
//
// Deviations:
// - The watchdog: RPCS3's thread restarts the game when a started watchdog isn't cleared within its timeout. Here
//   there is no restart; an expiry is logged (as an error) at the next watchdog call.
// - The board storage (arcade boards) is kept for the process; RPCS3 keeps it in a file across runs.
// - The OpenPSID is Config::console_psid_*, a fixed default, where RPCS3 makes a random one per installation.

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <memory>
#include <random>

#include "internal.hpp"
#include "pspocket/core/timebase.h"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kGame = "sys_game";
constexpr std::string_view kSs = "sys_ss";

struct GameState {
    std::mutex mutex;
    bool watchdog_active = false;
    u64 watchdog_timeout_us = 0;
    std::chrono::steady_clock::time_point watchdog_start;
    u64 sw_version = 0; // 0: the configured one
    std::array<u8, 16> board_storage = [] {
        std::array<u8, 16> a{};
        a.fill(0xFF); // RPCS3: all ones until written
        return a;
    }();
};

GameState& game_state(Kernel& kernel)
{
    Kernel::Impl& k = kernel.impl();
    std::lock_guard lock(k.system_mutex);
    if (!k.system_state) k.system_state = std::make_shared<GameState>();
    return *static_cast<GameState*>(k.system_state.get());
}

// A started watchdog whose time is up: RPCS3 restarts the game here; logged instead.
void check_watchdog(GameState& g)
{
    if (!g.watchdog_active) return;
    const auto elapsed = std::chrono::steady_clock::now() - g.watchdog_start;
    if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() <
        static_cast<s64>(g.watchdog_timeout_us))
        return;
    PS_LOG_ERROR(kGame, "watchdog timeout ({} us without a clear): RPCS3 restarts the game here; ignored",
        g.watchdog_timeout_us);
    g.watchdog_start = std::chrono::steady_clock::now();
}

} // namespace

// ------------------------------------------------------------------------------------------------- sys_game

// _sys_game_watchdog_start(timeout seconds): the firmware turns it into microseconds rounded down to 64.
Result _sys_game_watchdog_start(PpuThread& t, u32 timeout)
{
    GameState& g = game_state(t.kernel);
    std::lock_guard lock(g.mutex);
    if (g.watchdog_active) return err(CELL_EABORT);
    g.watchdog_active = true;
    g.watchdog_timeout_us = static_cast<u32>(timeout * 1'000'000u) & ~63u;
    g.watchdog_start = std::chrono::steady_clock::now();
    return CELL_OK;
}

Result _sys_game_watchdog_stop(PpuThread& t)
{
    GameState& g = game_state(t.kernel);
    std::lock_guard lock(g.mutex);
    check_watchdog(g);
    g.watchdog_active = false;
    return CELL_OK;
}

Result _sys_game_watchdog_clear(PpuThread& t)
{
    GameState& g = game_state(t.kernel);
    std::lock_guard lock(g.mutex);
    check_watchdog(g);
    if (g.watchdog_active) g.watchdog_start = std::chrono::steady_clock::now();
    return CELL_OK;
}

Result _sys_game_set_system_sw_version(PpuThread& t, u64 version)
{
    if (!t.kernel.config().root) return err(CELL_ENOSYS);
    GameState& g = game_state(t.kernel);
    std::lock_guard lock(g.mutex);
    g.sw_version = version;
    return CELL_OK;
}

// The version, not an error code (4.92 is 49200).
u64 _sys_game_get_system_sw_version(PpuThread& t)
{
    GameState& g = game_state(t.kernel);
    std::lock_guard lock(g.mutex);
    return g.sw_version ? g.sw_version : t.kernel.config().system_sw_version;
}

// The arcade board's 16 bytes of storage; *status 0 (read or written).
Result _sys_game_board_storage_read(PpuThread& t, u32 buffer, u32 status)
{
    Guest& guest = t.kernel.impl().guest;
    if (!buffer || !status) return err(CELL_EFAULT);
    if (!guest.valid(buffer, 16) || !guest.valid(status, 1)) return err(CELL_EFAULT);
    GameState& g = game_state(t.kernel);
    std::lock_guard lock(g.mutex);
    std::memcpy(guest.ptr(buffer), g.board_storage.data(), 16);
    ps_write8(guest.ptr(0), status, 0);
    return CELL_OK;
}

Result _sys_game_board_storage_write(PpuThread& t, u32 buffer, u32 status)
{
    Guest& guest = t.kernel.impl().guest;
    if (!buffer || !status) return err(CELL_EFAULT);
    if (!guest.valid(buffer, 16) || !guest.valid(status, 1)) return err(CELL_EFAULT);
    GameState& g = game_state(t.kernel);
    std::lock_guard lock(g.mutex);
    std::memcpy(g.board_storage.data(), guest.ptr(buffer), 16);
    ps_write8(guest.ptr(0), status, 0);
    return CELL_OK;
}

Result _sys_game_get_rtc_status(PpuThread& t, u32 status)
{
    if (!status) return err(CELL_EFAULT);
    if (!t.kernel.impl().guest.put32(status, 0)) return err(CELL_EFAULT);
    return CELL_OK;
}

// ------------------------------------------------------------------------------------------------- sys_ss

namespace {

constexpr u32 SYS_SS_RNG_ERROR_INVALID_PKG = 0x80010500, SYS_SS_RNG_ERROR_ENOMEM = 0x80010501;
constexpr u32 kBadPacketId = 0x80010500;
constexpr u32 kAceUnknownPackage = 0x8001051D;

} // namespace

// sys_ss_random_number_generator(pkg_id, buf, size): package 2 fills buf with random bytes (at most 256 MB); package
// 1 (root only) clears 0x18 bytes.
Result sys_ss_random_number_generator(PpuThread& t, u64 pkg_id, u32 buf, u64 size)
{
    Guest& guest = t.kernel.impl().guest;
    if (pkg_id == 1) {
        if (!t.kernel.config().root) return err(CELL_ENOSYS);
        if (!guest.valid(buf, 0x18)) return err(CELL_EFAULT);
        std::memset(guest.ptr(buf), 0, 0x18);
        return CELL_OK;
    }
    if (pkg_id != 2) return static_cast<Result>(SYS_SS_RNG_ERROR_INVALID_PKG);
    if (size > 0x10000000) return static_cast<Result>(SYS_SS_RNG_ERROR_ENOMEM);
    if (size && !guest.valid(buf, size)) return err(CELL_EFAULT);
    std::random_device rd;
    u8* out = guest.ptr(buf);
    for (u64 i = 0; i < size; i += 4) {
        const u32 v = rd();
        std::memcpy(out + i, &v, static_cast<usize>(std::min<u64>(4, size - i)));
    }
    return CELL_OK;
}

// sys_ss_appliance_info_manager(code, buf): root only. The device type, device ID (IDPS), PS code and OpenPSID are
// RPCS3's (a retail console).
Result sys_ss_appliance_info_manager(PpuThread& t, u32 code, u32 buffer)
{
    if (!t.kernel.config().root) return err(CELL_ENOSYS);
    Guest& guest = t.kernel.impl().guest;
    if (!buffer) return err(CELL_EFAULT);
    static constexpr u8 kProductCode[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x89};
    static constexpr u8 kIdps[16] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x89, 0x00, 0x0B, 0x14, 0x00, 0xEF, 0xDD, 0xCA,
        0x25, 0x52, 0x66};
    static constexpr u8 kPsCode[8] = {0x00, 0x01, 0x00, 0x85, 0x00, 0x07, 0x00, 0x04};
    switch (code) {
    case 0x19002:
        if (!guest.valid(buffer, 16)) return err(CELL_EFAULT);
        std::memcpy(guest.ptr(buffer), kProductCode, 16);
        break;
    case 0x19003:
        if (!guest.valid(buffer, 16)) return err(CELL_EFAULT);
        std::memcpy(guest.ptr(buffer), kIdps, 16);
        break;
    case 0x19004:
        if (!guest.valid(buffer, 8)) return err(CELL_EFAULT);
        std::memcpy(guest.ptr(buffer), kPsCode, 8);
        break;
    case 0x19005:
        if (!guest.valid(buffer, 16)) return err(CELL_EFAULT);
        guest.w64(buffer, t.kernel.config().console_psid_high);
        guest.w64(buffer + 8, t.kernel.config().console_psid_low);
        break;
    default: PS_LOG_WARN(kSs, "sys_ss_appliance_info_manager({:#x}): not known; nothing written (as RPCS3)", code); break;
    }
    return CELL_OK;
}

Result sys_ss_get_console_id(PpuThread& t, u32 buffer) { return sys_ss_appliance_info_manager(t, 0x19003, buffer); }

Result sys_ss_get_open_psid(PpuThread& t, u32 psid)
{
    Guest& guest = t.kernel.impl().guest;
    if (!guest.valid(psid, 16)) return err(CELL_EFAULT);
    guest.w64(psid, t.kernel.config().console_psid_high);
    guest.w64(psid + 8, t.kernel.config().console_psid_low);
    return CELL_OK;
}

// 0xFF: the hypervisor call failed (RPCS3: 0 makes something crash).
Result sys_ss_get_cache_of_product_mode(PpuThread& t, u32 ptr)
{
    if (!ptr) return err(CELL_EINVAL);
    Guest& guest = t.kernel.impl().guest;
    if (!guest.valid(ptr, 1)) return err(CELL_EFAULT);
    ps_write8(guest.ptr(0), ptr, 0xFF);
    return CELL_OK;
}

Result sys_ss_get_cache_of_flash_ext_flag(PpuThread& t, u32 flag)
{
    if (!flag) return err(CELL_EFAULT);
    if (!t.kernel.impl().guest.put64(flag, 0xFE)) return err(CELL_EFAULT); // NAND vs NOR from the lowest bit
    return CELL_OK;
}

Result sys_ss_get_boot_device(PpuThread& t, u32 dev)
{
    if (!dev) return err(CELL_EINVAL);
    if (!t.kernel.impl().guest.put64(dev, 0x190)) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_ss_secure_rtc(cmd, a2, a3, a4): 0x3002 reads the time base into *a3 with status 0 in *a4.
Result sys_ss_secure_rtc(PpuThread& t, u64 cmd, u64 a2, u64 a3, u64 a4)
{
    Guest& guest = t.kernel.impl().guest;
    switch (cmd) {
    case 0x3001: return a3 == 0x20 ? CELL_OK : static_cast<Result>(kBadPacketId);
    case 0x3002:
        if (a2 > 1) return static_cast<Result>(kBadPacketId);
        if (!guest.put64(static_cast<u32>(a3), ps_timebase_now()) || !guest.put64(static_cast<u32>(a4), 0)) {
            return err(CELL_EFAULT);
        }
        return CELL_OK;
    case 0x3003: return CELL_OK;
    default: return static_cast<Result>(kBadPacketId);
    }
}

// sys_ss_individual_info_manager(pkg_id, a2, out_size, a4, a5, a6): the EID size (0x100), and the read's size written
// back (RPCS3 reads no EID).
Result sys_ss_individual_info_manager(PpuThread& t, u64 pkg_id, u64, u32 out_size, u64 a4, u64 a5, u64)
{
    Guest& guest = t.kernel.impl().guest;
    if (pkg_id == 0x17002) {
        if (!guest.put64(static_cast<u32>(a5), a4)) return err(CELL_EFAULT);
    } else if (pkg_id == 0x17001) {
        if (!guest.put64(out_size, 0x100)) return err(CELL_EFAULT);
    }
    return CELL_OK;
}

// sys_ss_access_control_engine(pkg_id, a2, a3): the process's authority ID (0: pspocket has no SELF header to take it
// from at this level).
Result sys_ss_access_control_engine(PpuThread& t, u64 pkg_id, u64 a2, u64 a3)
{
    Guest& guest = t.kernel.impl().guest;
    constexpr u64 kAuthId = 0;
    switch (pkg_id) {
    case 1:
        if (!t.kernel.config().root) return err(CELL_ENOSYS);
        if (!a2) return err(CELL_ESRCH);
        if (!guest.put64(static_cast<u32>(a3), kAuthId)) return err(CELL_EFAULT);
        return CELL_OK;
    case 2:
        if (!guest.put64(static_cast<u32>(a2), kAuthId)) return err(CELL_EFAULT);
        return CELL_OK;
    case 3: return t.kernel.config().root ? CELL_OK : err(CELL_ENOSYS);
    default: return static_cast<Result>(kAceUnknownPackage);
    }
}

void register_system_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 372, _sys_game_watchdog_start);
    PS_LV2_BIND(table, 373, _sys_game_watchdog_stop);
    PS_LV2_BIND(table, 374, _sys_game_watchdog_clear);
    PS_LV2_BIND(table, 375, _sys_game_set_system_sw_version);
    PS_LV2_BIND(table, 376, _sys_game_get_system_sw_version);
    PS_LV2_BIND(table, 410, _sys_game_board_storage_read);
    PS_LV2_BIND(table, 411, _sys_game_board_storage_write);
    PS_LV2_BIND(table, 412, _sys_game_get_rtc_status);
    PS_LV2_BIND(table, 865, sys_ss_random_number_generator);
    PS_LV2_BIND(table, 866, sys_ss_secure_rtc);
    PS_LV2_BIND(table, 867, sys_ss_appliance_info_manager);
    PS_LV2_BIND(table, 868, sys_ss_individual_info_manager);
    PS_LV2_BIND(table, 870, sys_ss_get_console_id);
    PS_LV2_BIND(table, 871, sys_ss_access_control_engine);
    PS_LV2_BIND(table, 872, sys_ss_get_open_psid);
    PS_LV2_BIND(table, 873, sys_ss_get_cache_of_product_mode);
    PS_LV2_BIND(table, 874, sys_ss_get_cache_of_flash_ext_flag);
    PS_LV2_BIND(table, 875, sys_ss_get_boot_device);
}

} // namespace pspocket::lv2
